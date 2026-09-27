/*
 * PROJECT:     WinDosDX HD Audio codec driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Codec graph, output routing, stream and volume
 *
 * At start the codec's audio function group is read widget by widget. Every
 * analog output pin the BIOS configured (speaker, headphone, line out) gets
 * a route to a DAC through the mixers and selectors between them, preferring
 * a DAC no other pin uses. The routes are powered, unmuted at 0 dB and
 * enabled; all DACs play the same stream, so speaker and headphone both
 * sound. The master volume is applied at the DAC output amplifiers.
 */

#include "hdacodec.h"

#define NDEBUG
#include <debug.h>

/* ---------------------------------------------------------------------- */
/* IUnknown                                                                 */
/* ---------------------------------------------------------------------- */

STDMETHODIMP
CHdaCodec::QueryInterface(REFIID Iid, PVOID *Object)
{
    if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IHdaCodec))
    {
        *Object = this;
        AddRef();
        return STATUS_SUCCESS;
    }
    *Object = NULL;
    return STATUS_INVALID_PARAMETER;
}

STDMETHODIMP_(ULONG)
CHdaCodec::AddRef()
{
    return InterlockedIncrement(&m_Ref);
}

STDMETHODIMP_(ULONG)
CHdaCodec::Release()
{
    LONG Ref = InterlockedDecrement(&m_Ref);
    if (Ref == 0)
        delete this;
    return Ref;
}

CHdaCodec::~CHdaCodec()
{
    StopStream();
    if (m_HaveBus && m_Bus.InterfaceDereference)
        m_Bus.InterfaceDereference(m_Bus.Context);
}

/* ---------------------------------------------------------------------- */
/* Verbs                                                                    */
/* ---------------------------------------------------------------------- */

/* A 12-bit verb with 8 bits of data; Response gets the codec's answer. */
NTSTATUS
CHdaCodec::Verb(ULONG Node, ULONG VerbId, ULONG Data, PULONG Response)
{
    HDAUDIO_CODEC_TRANSFER Transfer;
    NTSTATUS Status;

    RtlZeroMemory(&Transfer, sizeof(Transfer));
    Transfer.Output.Command = ((ULONG)m_Codec << 28) | ((Node & 0xFF) << 20) |
                              ((VerbId & 0xFFF) << 8) | (Data & 0xFF);
    Status = m_Bus.TransferCodecVerbs(m_Bus.Context, 1, &Transfer, NULL, NULL);
    if (NT_SUCCESS(Status) && !Transfer.Input.IsValid)
        Status = STATUS_IO_DEVICE_ERROR;
    if (Response)
        *Response = NT_SUCCESS(Status) ? Transfer.Input.Response : 0;
    return Status;
}

/* A 4-bit verb with a 16-bit payload (converter format, amplifier gain). */
NTSTATUS
CHdaCodec::Verb4(ULONG Node, ULONG VerbId, ULONG Payload)
{
    HDAUDIO_CODEC_TRANSFER Transfer;

    RtlZeroMemory(&Transfer, sizeof(Transfer));
    Transfer.Output.Command = ((ULONG)m_Codec << 28) | ((Node & 0xFF) << 20) |
                              ((VerbId & 0xF) << 16) | (Payload & 0xFFFF);
    return m_Bus.TransferCodecVerbs(m_Bus.Context, 1, &Transfer, NULL, NULL);
}

ULONG
CHdaCodec::Parameter(ULONG Node, ULONG Param)
{
    ULONG Value = 0;
    Verb(Node, VERB_GET_PARAMETER, Param, &Value);
    return Value;
}

/* ---------------------------------------------------------------------- */
/* Start: bus interface and codec graph                                     */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QueryBusInterface(PDEVICE_OBJECT Pdo, PHDAUDIO_BUS_INTERFACE Bus)
{
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    PIO_STACK_LOCATION Stack;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, Pdo, NULL, 0, NULL, &Event, &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    Stack->Parameters.QueryInterface.InterfaceType = &GUID_HDAUDIO_BUS_INTERFACE;
    Stack->Parameters.QueryInterface.Size = sizeof(*Bus);
    Stack->Parameters.QueryInterface.Version = 0x0100;
    Stack->Parameters.QueryInterface.Interface = (PINTERFACE)Bus;
    Stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    Status = IoCallDriver(Pdo, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }
    return Status;
}

VOID
CHdaCodec::ReadWidget(UCHAR Node)
{
    PHDA_WIDGET W = &m_Widgets[Node];
    ULONG Length, Count, i;
    BOOLEAN Long;
    UCHAR Previous = 0;

    W->Caps = Parameter(Node, PARAM_AUDIO_CAPS);
    W->Type = (UCHAR)((W->Caps >> 20) & 0xF);
    W->AmpOutCaps = (W->Caps & WCAP_AMP_OVERRIDE) ? Parameter(Node, PARAM_OUTPUT_AMP_CAPS) : m_AfgAmpOutCaps;
    W->AmpInCaps = (W->Caps & WCAP_AMP_OVERRIDE) ? Parameter(Node, PARAM_INPUT_AMP_CAPS) : m_AfgAmpInCaps;
    if (W->Type == WIDGET_PIN)
    {
        W->PinCaps = Parameter(Node, PARAM_PIN_CAPS);
        Verb(Node, VERB_GET_CONFIG_DEFAULT, 0, &W->Config);
    }

    if (!(W->Caps & WCAP_CONN_LIST))
        return;
    Length = Parameter(Node, PARAM_CONNECTION_LENGTH);
    Long = (Length & 0x80) != 0;
    Count = Length & 0x7F;

    /* Short form: four 8-bit entries per response; long form: two 16-bit.
       An entry with the range bit set means every node since the last one. */
    for (i = 0; i < Count && W->ConnectionCount < HDA_MAX_CONNECTIONS; )
    {
        ULONG Response = 0, PerResponse = Long ? 2 : 4, j;
        Verb(Node, VERB_GET_CONNECTION_ENTRY, i, &Response);
        for (j = 0; j < PerResponse && i < Count; j++, i++)
        {
            ULONG Entry = Long ? (Response >> (16 * j)) & 0xFFFF : (Response >> (8 * j)) & 0xFF;
            BOOLEAN Range = Long ? (Entry & 0x8000) != 0 : (Entry & 0x80) != 0;
            UCHAR Id = (UCHAR)(Long ? Entry & 0x7FFF : Entry & 0x7F);

            if (Range && Previous && Id > Previous)
            {
                UCHAR n;
                for (n = Previous + 1; n <= Id && W->ConnectionCount < HDA_MAX_CONNECTIONS; n++)
                    W->Connections[W->ConnectionCount++] = n;
            }
            else if (W->ConnectionCount < HDA_MAX_CONNECTIONS)
            {
                W->Connections[W->ConnectionCount++] = Id;
            }
            Previous = Id;
        }
    }
}

/* Depth-first search from Nodes[Depth] to an analog DAC. */
static BOOLEAN
SearchDac(CHdaCodec *Codec, PHDA_WIDGET Widgets, UCHAR First, UCHAR Last,
          PHDA_PATH Path, UCHAR Depth, UCHAR AvoidDac, BOOLEAN AllowShared)
{
    PHDA_WIDGET W = &Widgets[Path->Nodes[Depth]];
    UCHAR i;

    UNREFERENCED_PARAMETER(Codec);
    for (i = 0; i < W->ConnectionCount; i++)
    {
        UCHAR Next = W->Connections[i];
        PHDA_WIDGET N;

        if (Next < First || Next > Last)
            continue;
        N = &Widgets[Next];
        Path->Select[Depth] = i;
        Path->Nodes[Depth + 1] = Next;
        if (N->Type == WIDGET_OUTPUT && !(N->Caps & WCAP_DIGITAL))
        {
            if (AllowShared || Next != AvoidDac)
            {
                Path->Count = Depth + 2;
                return TRUE;
            }
            continue;
        }
        if ((N->Type == WIDGET_MIXER || N->Type == WIDGET_SELECTOR) && Depth + 2 < HDA_MAX_PATH &&
            SearchDac(Codec, Widgets, First, Last, Path, Depth + 1, AvoidDac, AllowShared))
        {
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN
CHdaCodec::FindPath(UCHAR Pin, PHDA_PATH Path, UCHAR AvoidDac)
{
    UCHAR Last = m_FirstNode + m_NodeCount - 1;

    RtlZeroMemory(Path, sizeof(*Path));
    Path->Nodes[0] = Pin;
    Path->Device = (UCHAR)((m_Widgets[Pin].Config >> 20) & 0xF);
    /* A DAC of its own first; sharing one is the fallback. */
    return SearchDac(this, m_Widgets, m_FirstNode, Last, Path, 0, AvoidDac, FALSE) ||
           SearchDac(this, m_Widgets, m_FirstNode, Last, Path, 0, 0, TRUE);
}

/* Amplifier gain/mute (verb 3): bit 15 output, 14 input, 13 left, 12 right. */
VOID
CHdaCodec::SetAmp(UCHAR Node, BOOLEAN Output, UCHAR Index, BOOLEAN Mute, ULONG Gain)
{
    ULONG Payload = (Output ? 0x8000 : 0x4000) | 0x3000 | ((ULONG)(Index & 0xF) << 8) |
                    (Mute ? 0x80 : 0) | (Gain & 0x7F);
    Verb4(Node, VERB4_SET_AMP_GAIN_MUTE, Payload);
}

VOID
CHdaCodec::SetupPath(PHDA_PATH Path)
{
    UCHAR i;

    for (i = 0; i < Path->Count; i++)
    {
        UCHAR Node = Path->Nodes[i];
        PHDA_WIDGET W = &m_Widgets[Node];

        Verb(Node, VERB_SET_POWER_STATE, 0);    /* D0 */

        /* Pins and selectors pick one input; a mixer sums them, so the
           other inputs are muted. */
        if (i + 1 < Path->Count && W->ConnectionCount > 1 && W->Type != WIDGET_MIXER)
            Verb(Node, VERB_SET_CONNECTION_SELECT, Path->Select[i]);
        if (i + 1 < Path->Count && (W->Caps & WCAP_IN_AMP))
        {
            UCHAR c;
            if (W->Type == WIDGET_MIXER)
            {
                for (c = 0; c < W->ConnectionCount && c < 16; c++)
                    SetAmp(Node, FALSE, c, c != Path->Select[i], W->AmpInCaps & 0x7F);
            }
            else
            {
                SetAmp(Node, FALSE, Path->Select[i], FALSE, W->AmpInCaps & 0x7F);
            }
        }
        if (W->Caps & WCAP_OUT_AMP)
            SetAmp(Node, TRUE, 0, FALSE, W->AmpOutCaps & 0x7F);    /* 0 dB */

        if (W->Type == WIDGET_PIN)
        {
            ULONG Control = PINCTL_OUT_EN;
            if (Path->Device == DEVICE_HP_OUT && (W->PinCaps & PINCAP_HP_DRIVE))
                Control |= PINCTL_HP_EN;
            Verb(Node, VERB_SET_PIN_CONTROL, Control);
            /* External amplifier (laptop speakers). */
            if (W->PinCaps & PINCAP_EAPD)
                Verb(Node, VERB_SET_EAPD_BTL, 0x02);
        }
    }
}

NTSTATUS
CHdaCodec::Init(PDEVICE_OBJECT DeviceObject)
{
    PDEVICE_OBJECT Pdo;
    NTSTATUS Status;
    ULONG Nodes, Vendor, Type, n;
    UCHAR Pin;

    m_DeviceObject = DeviceObject;
    m_Volume = 0;
    m_Mute = FALSE;

    Status = PcGetPhysicalDeviceObject(DeviceObject, &Pdo);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(&m_Bus, sizeof(m_Bus));
    m_Bus.Size = sizeof(m_Bus);
    m_Bus.Version = 0x0100;
    Status = QueryBusInterface(Pdo, &m_Bus);
    if (!NT_SUCCESS(Status) || !m_Bus.TransferCodecVerbs)
    {
        DPRINT1("hdaudio: no HD Audio bus interface (0x%lx)\n", Status);
        return NT_SUCCESS(Status) ? STATUS_NOT_SUPPORTED : Status;
    }
    m_HaveBus = TRUE;

    m_Bus.GetResourceInformation(m_Bus.Context, &m_Codec, &m_Afg);
    Vendor = Parameter(0, PARAM_VENDOR_ID);
    Type = Parameter(m_Afg, PARAM_FUNCTION_TYPE) & 0xFF;
    if (Type != 0x01)   /* audio function group */
    {
        DPRINT1("hdaudio: codec %u node %u is not an audio function (type %lu)\n", m_Codec, m_Afg, Type);
        return STATUS_NOT_SUPPORTED;
    }

    Verb(m_Afg, VERB_SET_POWER_STATE, 0);
    m_AfgAmpOutCaps = Parameter(m_Afg, PARAM_OUTPUT_AMP_CAPS);
    m_AfgAmpInCaps = Parameter(m_Afg, PARAM_INPUT_AMP_CAPS);
    m_Rates = Parameter(m_Afg, PARAM_PCM);

    Nodes = Parameter(m_Afg, PARAM_NODE_COUNT);
    m_FirstNode = (UCHAR)((Nodes >> 16) & 0xFF);
    m_NodeCount = (UCHAR)(Nodes & 0xFF);
    if (!m_NodeCount || m_FirstNode + m_NodeCount > HDA_MAX_NODES)
        return STATUS_NOT_SUPPORTED;
    for (n = m_FirstNode; n < (ULONG)m_FirstNode + m_NodeCount; n++)
        ReadWidget((UCHAR)n);

    /* Analog output pins the BIOS wired up, speaker and headphone first. */
    m_PathCount = 0;
    for (ULONG Wanted = 0; Wanted < 3; Wanted++)
    {
        static const UCHAR Order[3] = { DEVICE_SPEAKER, DEVICE_HP_OUT, DEVICE_LINE_OUT };
        for (Pin = m_FirstNode; Pin < m_FirstNode + m_NodeCount && m_PathCount < HDA_MAX_OUTPUTS; Pin++)
        {
            PHDA_WIDGET W = &m_Widgets[Pin];
            HDA_PATH Path;
            UCHAR UsedDac = 0;

            if (W->Type != WIDGET_PIN || !(W->PinCaps & PINCAP_OUTPUT) || (W->Caps & WCAP_DIGITAL))
                continue;
            if (((W->Config >> 30) & 0x3) == CONNECT_NONE || ((W->Config >> 20) & 0xF) != Order[Wanted])
                continue;
            if (m_PathCount)
                UsedDac = m_Paths[m_PathCount - 1].Nodes[m_Paths[m_PathCount - 1].Count - 1];
            if (FindPath(Pin, &Path, UsedDac))
                m_Paths[m_PathCount++] = Path;
        }
    }
    if (!m_PathCount)
    {
        DPRINT1("hdaudio: codec %08lx has no analog output route\n", Vendor);
        return STATUS_NOT_SUPPORTED;
    }

    /* The sample rates every DAC in use supports. */
    for (n = 0; n < m_PathCount; n++)
    {
        UCHAR Dac = m_Paths[n].Nodes[m_Paths[n].Count - 1];
        ULONG Pcm = Parameter(Dac, PARAM_PCM);
        if (Pcm & 0xFFF)
            m_Rates &= Pcm;
        SetupPath(&m_Paths[n]);
    }
    if (!(m_Rates & (PCM_RATE_44100 | PCM_RATE_48000)))
        m_Rates |= PCM_RATE_48000;  /* every codec must do 48 kHz */

    DPRINT1("hdaudio: codec %08lx, %u output route(s), rates %03lx\n", Vendor, m_PathCount, m_Rates & 0xFFF);
    ApplyVolume();
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Stream                                                                   */
/* ---------------------------------------------------------------------- */

NTSTATUS
CHdaCodec::StartStream(ULONG SampleRate, ULONG BufferSize, PVOID *Buffer, PULONG AllocatedSize)
{
    HDAUDIO_STREAM_FORMAT Format;
    ULONG Fifo = 0;
    NTSTATUS Status;
    UCHAR n;

    StopStream();

    Format.SampleRate = SampleRate;
    Format.ValidBitsPerSample = 16;
    Format.ContainerSize = 16;
    Format.NumberOfChannels = 2;
    Status = m_Bus.AllocateRenderDmaEngine(m_Bus.Context, &Format, FALSE, &m_DmaEngine, &m_Format);
    if (!NT_SUCCESS(Status))
    {
        m_DmaEngine = NULL;
        return Status;
    }
    Status = m_Bus.AllocateDmaBuffer(m_Bus.Context, m_DmaEngine, BufferSize, &m_BufferMdl,
                                     &m_BufferSize, &m_StreamId, &Fifo);
    if (NT_SUCCESS(Status))
    {
        *Buffer = MmGetSystemAddressForMdlSafe(m_BufferMdl, NormalPagePriority);
        if (!*Buffer)
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(Status))
        Status = m_Bus.GetLinkPositionRegister(m_Bus.Context, m_DmaEngine, &m_Position);
    if (!NT_SUCCESS(Status))
    {
        StopStream();
        return Status;
    }
    RtlZeroMemory(*Buffer, m_BufferSize);
    *AllocatedSize = (ULONG)m_BufferSize;

    /* Every DAC in use takes the stream, channels 0 and 1. */
    for (n = 0; n < m_PathCount; n++)
    {
        UCHAR Dac = m_Paths[n].Nodes[m_Paths[n].Count - 1];
        Verb4(Dac, VERB4_SET_CONVERTER_FORMAT, m_Format.ConverterFormat);
        Verb(Dac, VERB_SET_STREAM_CHANNEL, ((ULONG)m_StreamId << 4) | 0);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
CHdaCodec::RunStream(BOOLEAN Run)
{
    NTSTATUS Status;
    if (!m_DmaEngine)
        return STATUS_INVALID_DEVICE_STATE;
    Status = m_Bus.SetDmaEngineState(m_Bus.Context, Run ? RunState : PauseState, 1, &m_DmaEngine);
    return Status;
}

VOID
CHdaCodec::StopStream()
{
    UCHAR n;

    if (!m_DmaEngine)
        return;
    m_Bus.SetDmaEngineState(m_Bus.Context, ResetState, 1, &m_DmaEngine);
    for (n = 0; n < m_PathCount; n++)
        Verb(m_Paths[n].Nodes[m_Paths[n].Count - 1], VERB_SET_STREAM_CHANNEL, 0);
    if (m_BufferMdl)
        m_Bus.FreeDmaBuffer(m_Bus.Context, m_DmaEngine);
    m_Bus.FreeDmaEngine(m_Bus.Context, m_DmaEngine);
    m_DmaEngine = NULL;
    m_BufferMdl = NULL;
    m_BufferSize = 0;
    m_Position = NULL;
}

/* The DMA engine's position in the cyclic buffer (bytes). */
ULONG
CHdaCodec::StreamPosition()
{
    ULONG Position;

    if (!m_Position || !m_BufferSize)
        return 0;
    Position = READ_REGISTER_ULONG(m_Position);
    return Position % (ULONG)m_BufferSize;
}

/* ---------------------------------------------------------------------- */
/* Volume                                                                   */
/* ---------------------------------------------------------------------- */

/* Levels are in 1/65536 dB, as KSPROPERTY_AUDIO_VOLUMELEVEL. */
#define VOLUME_MIN  (-96 * 65536)

VOID
CHdaCodec::GetVolumeRange(PLONG Minimum, PLONG Maximum, PULONG Step)
{
    *Minimum = VOLUME_MIN;
    *Maximum = 0;
    *Step = 65536 / 2;  /* 0.5 dB */
}

VOID
CHdaCodec::SetVolume(LONG Level)
{
    m_Volume = max(VOLUME_MIN, min(0, Level));
    ApplyVolume();
}

VOID
CHdaCodec::SetMute(BOOLEAN Mute)
{
    m_Mute = Mute;
    ApplyVolume();
}

/* The first amplifier with steps on each route, seen from the DAC, carries
   the master volume. Amplifier caps: bits 6:0 the step for 0 dB, 14:8 the
   number of steps, 22:16 the step size in 0.25 dB units minus one. */
VOID
CHdaCodec::ApplyVolume()
{
    UCHAR n;

    for (n = 0; n < m_PathCount; n++)
    {
        PHDA_PATH Path = &m_Paths[n];
        LONG i;

        for (i = Path->Count - 1; i >= 0; i--)
        {
            PHDA_WIDGET W = &m_Widgets[Path->Nodes[i]];
            ULONG Caps = W->AmpOutCaps, Offset, Steps, StepQuarterDb;
            LONG Gain;

            if (!(W->Caps & WCAP_OUT_AMP) || !((Caps >> 8) & 0x7F))
                continue;
            Offset = Caps & 0x7F;
            Steps = (Caps >> 8) & 0x7F;
            StepQuarterDb = ((Caps >> 16) & 0x7F) + 1;
            /* dB = (gain - offset) * step; the level is at most 0 dB. */
            Gain = (LONG)Offset + (LONG)((m_Volume / 16384) / (LONG)StepQuarterDb);
            Gain = max(0, min((LONG)Steps, Gain));
            SetAmp(Path->Nodes[i], TRUE, 0, m_Mute, (ULONG)Gain);
            break;
        }
        /* No amplifier with steps: mute by switching the pin off. */
        if (i < 0)
        {
            ULONG Control = m_Mute ? 0 : PINCTL_OUT_EN;
            if (!m_Mute && Path->Device == DEVICE_HP_OUT &&
                (m_Widgets[Path->Nodes[0]].PinCaps & PINCAP_HP_DRIVE))
                Control |= PINCTL_HP_EN;
            Verb(Path->Nodes[0], VERB_SET_PIN_CONTROL, Control);
        }
    }
}
