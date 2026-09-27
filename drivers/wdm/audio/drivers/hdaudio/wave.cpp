/*
 * PROJECT:     WinDosDX HD Audio codec driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WaveCyclic miniport: one 16-bit stereo render stream
 *
 * The cyclic buffer is the bus driver's DMA buffer, handed to PortCls through
 * a small IDmaChannel wrapper; PortCls fills it and reads the play position
 * from the stream's link position register. A 10 ms timer asks PortCls to
 * service the stream, as a DMA interrupt would.
 */

#include "hdacodec.h"

#define NDEBUG
#include <debug.h>

#define HDA_BUFFER_SIZE     (16 * 1024)

/* ---------------------------------------------------------------------- */
/* Filter description                                                       */
/* ---------------------------------------------------------------------- */

static KSDATARANGE BridgeRange =
{
    sizeof(KSDATARANGE), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE BridgeRanges[] = { &BridgeRange };

/* The rates are narrowed to what the codec plays in CMiniportWave::Init. */
static KSDATARANGE_AUDIO RenderRange =
{
    {
        sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    2,          /* MaximumChannels */
    16, 16,     /* Minimum/MaximumBitsPerSample */
    44100, 48000
};
static PKSDATARANGE RenderRanges[] = { (PKSDATARANGE)&RenderRange };

static PCPIN_DESCRIPTOR WavePins[] =
{
    /* WAVE_PIN_RENDER */
    {
        1, 1, 0, NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(RenderRanges), RenderRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK,
            (GUID *)&KSCATEGORY_AUDIO, NULL, 0
        }
    },
    /* WAVE_PIN_BRIDGE */
    {
        0, 0, 0, NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(BridgeRanges), BridgeRanges,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
            (GUID *)&KSCATEGORY_AUDIO, NULL, 0
        }
    }
};

static PCNODE_DESCRIPTOR WaveNodes[] =
{
    { 0, NULL, &KSNODETYPE_DAC, NULL }
};

/* A node's input is pin 1 and its output pin 0 (KSNODEPIN_STANDARD_*). */
static PCCONNECTION_DESCRIPTOR WaveConnections[] =
{
    { PCFILTER_NODE, WAVE_PIN_RENDER, 0, 1 },
    { 0, 0, PCFILTER_NODE, WAVE_PIN_BRIDGE }
};

static PCFILTER_DESCRIPTOR WaveFilter =
{
    0, NULL,
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(WavePins), WavePins,
    sizeof(PCNODE_DESCRIPTOR), SIZEOF_ARRAY(WaveNodes), WaveNodes,
    SIZEOF_ARRAY(WaveConnections), WaveConnections,
    0, NULL
};

/* ---------------------------------------------------------------------- */
/* IDmaChannel over the bus driver's DMA buffer                             */
/* ---------------------------------------------------------------------- */

class CHdaDmaChannel : public IDmaChannel
{
public:
    CHdaDmaChannel(PVOID Buffer, ULONG Size) : m_Ref(1), m_Buffer(Buffer), m_Size(Size), m_Allocated(Size) {}

    STDMETHODIMP QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IDmaChannel))
        {
            *Object = PVOID(PDMACHANNEL(this));
            AddRef();
            return STATUS_SUCCESS;
        }
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG Ref = InterlockedDecrement(&m_Ref);
        if (Ref == 0)
            delete this;
        return Ref;
    }

    STDMETHODIMP_(NTSTATUS) AllocateBuffer(ULONG BufferSize, PPHYSICAL_ADDRESS Constraint)
    {
        UNREFERENCED_PARAMETER(Constraint);
        return BufferSize <= m_Allocated ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }
    STDMETHODIMP_(void) FreeBuffer() {}
    STDMETHODIMP_(ULONG) TransferCount() { return m_Allocated; }
    STDMETHODIMP_(ULONG) MaximumBufferSize() { return m_Allocated; }
    STDMETHODIMP_(ULONG) AllocatedBufferSize() { return m_Allocated; }
    STDMETHODIMP_(ULONG) BufferSize() { return m_Size; }
    STDMETHODIMP_(void) SetBufferSize(ULONG BufferSize) { m_Size = min(BufferSize, m_Allocated); }
    STDMETHODIMP_(PVOID) SystemAddress() { return m_Buffer; }
#if defined(__cplusplus) && !defined(_MSC_VER)
    STDMETHODIMP_(PHYSICAL_ADDRESS *) PhysicalAddress(PHYSICAL_ADDRESS *Ret)
    {
        Ret->QuadPart = 0;  /* the bus driver owns the DMA mapping */
        return Ret;
    }
#else
    STDMETHODIMP_(PHYSICAL_ADDRESS) PhysicalAddress()
    {
        PHYSICAL_ADDRESS None;
        None.QuadPart = 0;  /* the bus driver owns the DMA mapping */
        return None;
    }
#endif
    STDMETHODIMP_(PADAPTER_OBJECT) GetAdapterObject() { return NULL; }
    STDMETHODIMP_(void) CopyTo(PVOID Destination, PVOID Source, ULONG ByteCount)
    {
        RtlCopyMemory(Destination, Source, ByteCount);
    }
    STDMETHODIMP_(void) CopyFrom(PVOID Destination, PVOID Source, ULONG ByteCount)
    {
        RtlCopyMemory(Destination, Source, ByteCount);
    }

private:
    LONG m_Ref;
    PVOID m_Buffer;
    ULONG m_Size;
    ULONG m_Allocated;
};

/* ---------------------------------------------------------------------- */
/* Stream                                                                   */
/* ---------------------------------------------------------------------- */

class CWaveStream : public IMiniportWaveCyclicStream
{
public:
    CWaveStream() : m_Ref(1) {}

    STDMETHODIMP QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IMiniportWaveCyclicStream))
        {
            *Object = PVOID(PMINIPORTWAVECYCLICSTREAM(this));
            AddRef();
            return STATUS_SUCCESS;
        }
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG Ref = InterlockedDecrement(&m_Ref);
        if (Ref == 0)
            delete this;
        return Ref;
    }

    IMP_IMiniportWaveCyclicStream;

    NTSTATUS Init(PHDACODEC Codec, PSERVICEGROUP ServiceGroup, ULONG SampleRate,
                  PLONG ActiveFlag, PDMACHANNEL *DmaChannel);

private:
    ~CWaveStream();
    static VOID NTAPI TimerDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2);

    LONG m_Ref;
    PHDACODEC m_Codec;
    PSERVICEGROUP m_ServiceGroup;
    CHdaDmaChannel *m_Dma;
    PLONG m_ActiveFlag;
    ULONG m_SampleRate;
    ULONG m_BytesPerSecond;
    ULONG m_Interval;
    KTIMER m_Timer;
    KDPC m_Dpc;
    BOOLEAN m_Running;
};

NTSTATUS
CWaveStream::Init(PHDACODEC Codec, PSERVICEGROUP ServiceGroup, ULONG SampleRate,
                  PLONG ActiveFlag, PDMACHANNEL *DmaChannel)
{
    PVOID Buffer = NULL;
    ULONG Size = 0;
    NTSTATUS Status;

    m_Codec = Codec;
    m_Codec->AddRef();
    m_ServiceGroup = ServiceGroup;
    m_ServiceGroup->AddRef();
    m_ActiveFlag = ActiveFlag;
    m_SampleRate = SampleRate;
    m_BytesPerSecond = SampleRate * 4;
    m_Interval = 10;
    KeInitializeTimerEx(&m_Timer, NotificationTimer);
    KeInitializeDpc(&m_Dpc, TimerDpc, this);

    Status = m_Codec->StartStream(SampleRate, HDA_BUFFER_SIZE, &Buffer, &Size);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hdaudio: could not start a %lu Hz stream (0x%lx)\n", SampleRate, Status);
        return Status;
    }
    m_Dma = new (NonPagedPool, HDA_TAG) CHdaDmaChannel(Buffer, Size);
    if (!m_Dma)
        return STATUS_INSUFFICIENT_RESOURCES;
    m_Dma->AddRef();
    *DmaChannel = m_Dma;
    return STATUS_SUCCESS;
}

CWaveStream::~CWaveStream()
{
    KeCancelTimer(&m_Timer);
    KeFlushQueuedDpcs();
    if (m_Codec)
    {
        m_Codec->StopStream();
        m_Codec->Release();
    }
    if (m_Dma)
        m_Dma->Release();
    if (m_ServiceGroup)
        m_ServiceGroup->Release();
    if (m_ActiveFlag)
        InterlockedExchange(m_ActiveFlag, 0);
}

VOID NTAPI
CWaveStream::TimerDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    CWaveStream *This = (CWaveStream *)Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    This->m_ServiceGroup->RequestService();
}

STDMETHODIMP_(NTSTATUS)
CWaveStream::SetFormat(PKSDATAFORMAT DataFormat)
{
    PWAVEFORMATEX Format = DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX) ?
                           &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx : NULL;

    /* The DMA buffer is set up for one rate; the same format is fine. */
    if (Format && Format->nSamplesPerSec == m_SampleRate && Format->nChannels == 2 &&
        Format->wBitsPerSample == 16)
    {
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

STDMETHODIMP_(ULONG)
CWaveStream::SetNotificationFreq(ULONG Interval, PULONG FrameSize)
{
    m_Interval = Interval ? Interval : 10;
    *FrameSize = m_BytesPerSecond * m_Interval / 1000;
    return m_Interval;
}

STDMETHODIMP_(NTSTATUS)
CWaveStream::SetState(KSSTATE State)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (State == KSSTATE_RUN && !m_Running)
    {
        LARGE_INTEGER Due;
        Status = m_Codec->RunStream(TRUE);
        if (NT_SUCCESS(Status))
        {
            Due.QuadPart = -(LONGLONG)m_Interval * 10000;
            KeSetTimerEx(&m_Timer, Due, m_Interval, &m_Dpc);
            m_Running = TRUE;
        }
    }
    else if (State != KSSTATE_RUN && m_Running)
    {
        KeCancelTimer(&m_Timer);
        Status = m_Codec->RunStream(FALSE);
        m_Running = FALSE;
    }
    return Status;
}

STDMETHODIMP_(NTSTATUS)
CWaveStream::GetPosition(PULONG Position)
{
    *Position = m_Codec->StreamPosition();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CWaveStream::NormalizePhysicalPosition(PLONGLONG PhysicalPosition)
{
    /* Bytes to 100 ns units. */
    *PhysicalPosition = (*PhysicalPosition * 10000000) / m_BytesPerSecond;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(void)
CWaveStream::Silence(PVOID Buffer, ULONG ByteCount)
{
    RtlZeroMemory(Buffer, ByteCount);
}

/* ---------------------------------------------------------------------- */
/* Miniport                                                                 */
/* ---------------------------------------------------------------------- */

class CMiniportWave : public IMiniportWaveCyclic
{
public:
    CMiniportWave() : m_Ref(1) {}

    STDMETHODIMP QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IMiniport) ||
            IsEqualGUIDAligned(Iid, IID_IMiniportWaveCyclic))
        {
            *Object = PVOID(PMINIPORTWAVECYCLIC(this));
            AddRef();
            return STATUS_SUCCESS;
        }
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG Ref = InterlockedDecrement(&m_Ref);
        if (Ref == 0)
            delete this;
        return Ref;
    }

    IMP_IMiniportWaveCyclic;

private:
    ~CMiniportWave()
    {
        if (m_ServiceGroup)
            m_ServiceGroup->Release();
        if (m_Codec)
            m_Codec->Release();
    }

    LONG m_Ref;
    PHDACODEC m_Codec;
    PSERVICEGROUP m_ServiceGroup;
    LONG m_StreamActive;
};

STDMETHODIMP_(NTSTATUS)
CMiniportWave::Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTWAVECYCLIC Port)
{
    NTSTATUS Status;
    ULONG Rates;

    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);

    Status = UnknownAdapter->QueryInterface(IID_IHdaCodec, (PVOID *)&m_Codec);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = PcNewServiceGroup(&m_ServiceGroup, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Rates = m_Codec->SupportedRates();
    RenderRange.MinimumSampleFrequency = (Rates & PCM_RATE_44100) ? 44100 : 48000;
    RenderRange.MaximumSampleFrequency = (Rates & PCM_RATE_48000) ? 48000 : 44100;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWave::GetDescription(PPCFILTER_DESCRIPTOR *Description)
{
    *Description = &WaveFilter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWave::DataRangeIntersection(ULONG PinId, PKSDATARANGE ClientRange, PKSDATARANGE MyRange,
                                     ULONG OutputBufferLength, PVOID ResultantFormat,
                                     PULONG ResultantFormatLength)
{
    PKSDATARANGE_AUDIO Client = (PKSDATARANGE_AUDIO)ClientRange;
    PKSDATARANGE_AUDIO Mine = (PKSDATARANGE_AUDIO)MyRange;
    PKSDATAFORMAT_WAVEFORMATEX Result = (PKSDATAFORMAT_WAVEFORMATEX)ResultantFormat;
    ULONG Rate;

    if (PinId != WAVE_PIN_RENDER)
        return STATUS_NOT_IMPLEMENTED;
    if (IsEqualGUIDAligned(ClientRange->Specifier, KSDATAFORMAT_SPECIFIER_DSOUND))
        return STATUS_NOT_SUPPORTED;
    if (!OutputBufferLength || !ResultantFormat)
    {
        *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
        return STATUS_BUFFER_OVERFLOW;
    }
    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEX))
        return STATUS_BUFFER_TOO_SMALL;

    /* 48 kHz when both sides allow it, else 44.1 kHz. */
    if (ClientRange->FormatSize >= sizeof(KSDATARANGE_AUDIO))
    {
        if (Client->MaximumChannels < 2 || Client->MinimumBitsPerSample > 16 ||
            Client->MaximumBitsPerSample < 16)
            return STATUS_NO_MATCH;
        if (Mine->MaximumSampleFrequency >= 48000 && Client->MinimumSampleFrequency <= 48000 &&
            Client->MaximumSampleFrequency >= 48000)
            Rate = 48000;
        else if (Mine->MinimumSampleFrequency <= 44100 && Client->MinimumSampleFrequency <= 44100 &&
                 Client->MaximumSampleFrequency >= 44100)
            Rate = 44100;
        else
            return STATUS_NO_MATCH;
    }
    else
    {
        Rate = Mine->MaximumSampleFrequency;
    }

    Result->DataFormat = *MyRange;
    Result->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    Result->DataFormat.SampleSize = 4;
    Result->WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
    Result->WaveFormatEx.nChannels = 2;
    Result->WaveFormatEx.nSamplesPerSec = Rate;
    Result->WaveFormatEx.wBitsPerSample = 16;
    Result->WaveFormatEx.nBlockAlign = 4;
    Result->WaveFormatEx.nAvgBytesPerSec = Rate * 4;
    Result->WaveFormatEx.cbSize = 0;
    *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWave::NewStream(PMINIPORTWAVECYCLICSTREAM *Stream, PUNKNOWN OuterUnknown, POOL_TYPE PoolType,
                         ULONG Pin, BOOLEAN Capture, PKSDATAFORMAT DataFormat,
                         PDMACHANNEL *DmaChannel, PSERVICEGROUP *ServiceGroup)
{
    PWAVEFORMATEX Format = &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx;
    CWaveStream *WaveStream;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(OuterUnknown);
    UNREFERENCED_PARAMETER(PoolType);

    if (Capture || Pin != WAVE_PIN_RENDER)
        return STATUS_INVALID_PARAMETER;
    if (DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) || Format->nChannels != 2 ||
        Format->wBitsPerSample != 16 ||
        (Format->nSamplesPerSec != 44100 && Format->nSamplesPerSec != 48000))
    {
        return STATUS_INVALID_PARAMETER;
    }
    /* The codec plays one stream. */
    if (InterlockedCompareExchange(&m_StreamActive, 1, 0) != 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    WaveStream = new (NonPagedPool, HDA_TAG) CWaveStream();
    if (!WaveStream)
    {
        InterlockedExchange(&m_StreamActive, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = WaveStream->Init(m_Codec, m_ServiceGroup, Format->nSamplesPerSec, &m_StreamActive, DmaChannel);
    if (!NT_SUCCESS(Status))
    {
        WaveStream->Release();
        return Status;
    }

    m_ServiceGroup->AddRef();
    *ServiceGroup = m_ServiceGroup;
    *Stream = WaveStream;
    return STATUS_SUCCESS;
}

NTSTATUS
CreateWaveMiniport(PUNKNOWN *Unknown, REFCLSID ClassId, PUNKNOWN OuterUnknown, POOL_TYPE PoolType)
{
    CMiniportWave *Miniport;

    UNREFERENCED_PARAMETER(ClassId);
    UNREFERENCED_PARAMETER(OuterUnknown);

    Miniport = new (PoolType, HDA_TAG) CMiniportWave();
    if (!Miniport)
        return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = PUNKNOWN(PMINIPORTWAVECYCLIC(Miniport));
    return STATUS_SUCCESS;
}
