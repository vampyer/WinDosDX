/*
 * PROJECT:     WinDosDX HD Audio codec driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Topology miniport: master volume and mute
 *
 *   wave in (pin 0) --> volume --> mute --> speaker/line out (pin 1)
 *
 * Both controls act on the codec's output amplifiers (CHdaCodec::SetVolume
 * and SetMute); stereo channels share one setting.
 */

#include "hdacodec.h"

#define NDEBUG
#include <debug.h>

static NTSTATUS NTAPI PropertyVolume(PPCPROPERTY_REQUEST Request);
static NTSTATUS NTAPI PropertyMute(PPCPROPERTY_REQUEST Request);

static KSDATARANGE AnalogRange =
{
    sizeof(KSDATARANGE), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE AnalogRanges[] = { &AnalogRange };

static PCPIN_DESCRIPTOR TopoPins[] =
{
    /* TOPO_PIN_WAVEIN: from the wave filter's bridge pin */
    {
        0, 0, 0, NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(AnalogRanges), AnalogRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
            (GUID *)&KSCATEGORY_AUDIO, NULL, 0
        }
    },
    /* TOPO_PIN_LINEOUT: the speakers */
    {
        0, 0, 0, NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(AnalogRanges), AnalogRanges,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
            (GUID *)&KSNODETYPE_SPEAKER, NULL, 0
        }
    }
};

static PCPROPERTY_ITEM VolumeProperties[] =
{
    {
        &KSPROPSETID_Audio, KSPROPERTY_AUDIO_VOLUMELEVEL,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyVolume
    }
};
DEFINE_PCAUTOMATION_TABLE_PROP(VolumeAutomation, VolumeProperties);

static PCPROPERTY_ITEM MuteProperties[] =
{
    {
        &KSPROPSETID_Audio, KSPROPERTY_AUDIO_MUTE,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyMute
    }
};
DEFINE_PCAUTOMATION_TABLE_PROP(MuteAutomation, MuteProperties);

static PCNODE_DESCRIPTOR TopoNodes[] =
{
    { 0, &VolumeAutomation, &KSNODETYPE_VOLUME, &KSAUDFNAME_MASTER_VOLUME },
    { 0, &MuteAutomation, &KSNODETYPE_MUTE, &KSAUDFNAME_MASTER_MUTE }
};

static PCCONNECTION_DESCRIPTOR TopoConnections[] =
{
    { PCFILTER_NODE, TOPO_PIN_WAVEIN, TOPO_NODE_VOLUME, 1 },
    { TOPO_NODE_VOLUME, 0, TOPO_NODE_MUTE, 1 },
    { TOPO_NODE_MUTE, 0, PCFILTER_NODE, TOPO_PIN_LINEOUT }
};

static PCFILTER_DESCRIPTOR TopoFilter =
{
    0, NULL,
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(TopoPins), TopoPins,
    sizeof(PCNODE_DESCRIPTOR), SIZEOF_ARRAY(TopoNodes), TopoNodes,
    SIZEOF_ARRAY(TopoConnections), TopoConnections,
    0, NULL
};

class CMiniportTopo : public IMiniportTopology
{
public:
    CMiniportTopo() : m_Ref(1) {}

    STDMETHODIMP QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IMiniport) ||
            IsEqualGUIDAligned(Iid, IID_IMiniportTopology))
        {
            *Object = PVOID(PMINIPORTTOPOLOGY(this));
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

    IMP_IMiniportTopology;

    PHDACODEC m_Codec;

private:
    ~CMiniportTopo()
    {
        if (m_Codec)
            m_Codec->Release();
    }

    LONG m_Ref;
};

STDMETHODIMP_(NTSTATUS)
CMiniportTopo::Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTTOPOLOGY Port)
{
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return UnknownAdapter->QueryInterface(IID_IHdaCodec, (PVOID *)&m_Codec);
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopo::GetDescription(PPCFILTER_DESCRIPTOR *Description)
{
    *Description = &TopoFilter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopo::DataRangeIntersection(ULONG PinId, PKSDATARANGE ClientRange, PKSDATARANGE MyRange,
                                     ULONG OutputBufferLength, PVOID ResultantFormat,
                                     PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientRange);
    UNREFERENCED_PARAMETER(MyRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}

static PHDACODEC
RequestCodec(PPCPROPERTY_REQUEST Request)
{
    return ((CMiniportTopo *)PMINIPORTTOPOLOGY(Request->MajorTarget))->m_Codec;
}

/* Basic support: the value type and, for the volume, its range. */
static NTSTATUS
BasicSupport(PPCPROPERTY_REQUEST Request, ULONG VarType, BOOLEAN Ranged, LONG Minimum,
             LONG Maximum, ULONG Step)
{
    ULONG Full = sizeof(KSPROPERTY_DESCRIPTION) +
                 (Ranged ? sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG) : 0);
    ULONG Access = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;

    if (Request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        PKSPROPERTY_DESCRIPTION Description = (PKSPROPERTY_DESCRIPTION)Request->Value;
        Description->AccessFlags = Access;
        Description->DescriptionSize = Full;
        Description->PropTypeSet.Set = KSPROPTYPESETID_General;
        Description->PropTypeSet.Id = VarType;
        Description->PropTypeSet.Flags = 0;
        Description->MembersListCount = Ranged ? 1 : 0;
        Description->Reserved = 0;
        if (Ranged && Request->ValueSize >= Full)
        {
            PKSPROPERTY_MEMBERSHEADER Members = (PKSPROPERTY_MEMBERSHEADER)(Description + 1);
            PKSPROPERTY_STEPPING_LONG Range = (PKSPROPERTY_STEPPING_LONG)(Members + 1);
            Members->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
            Members->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
            Members->MembersCount = 1;
            Members->Flags = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_UNIFORM;
            Range->SteppingDelta = Step;
            Range->Reserved = 0;
            Range->Bounds.SignedMinimum = Minimum;
            Range->Bounds.SignedMaximum = Maximum;
            Request->ValueSize = Full;
        }
        else
        {
            Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        }
        return STATUS_SUCCESS;
    }
    if (Request->ValueSize >= sizeof(ULONG))
    {
        *(PULONG)Request->Value = Access;
        Request->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    Request->ValueSize = 0;
    return STATUS_BUFFER_TOO_SMALL;
}

/*
 * The volume and mute nodes are stereo: channel 0 (left), 1 (right) or -1
 * (all). Any other channel must fail, or mixer code that counts channels by
 * asking for one after another never stops.
 */
static BOOLEAN
ValidChannel(PPCPROPERTY_REQUEST Request)
{
    LONG Channel;

    if (!Request->Instance || Request->InstanceSize < sizeof(LONG))
        return TRUE;
    Channel = *(PLONG)Request->Instance;
    return Channel == 0 || Channel == 1 || Channel == -1;
}

static NTSTATUS NTAPI
PropertyVolume(PPCPROPERTY_REQUEST Request)
{
    PHDACODEC Codec = RequestCodec(Request);
    LONG Minimum, Maximum;
    ULONG Step;

    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        Codec->GetVolumeRange(&Minimum, &Maximum, &Step);
        return BasicSupport(Request, VT_I4, TRUE, Minimum, Maximum, Step);
    }
    if (!ValidChannel(Request))
        return STATUS_INVALID_PARAMETER;
    if (Request->ValueSize < sizeof(LONG))
        return STATUS_BUFFER_TOO_SMALL;
    if (Request->Verb & KSPROPERTY_TYPE_GET)
    {
        *(PLONG)Request->Value = Codec->GetVolume();
        Request->ValueSize = sizeof(LONG);
        return STATUS_SUCCESS;
    }
    if (Request->Verb & KSPROPERTY_TYPE_SET)
    {
        Codec->SetVolume(*(PLONG)Request->Value);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS NTAPI
PropertyMute(PPCPROPERTY_REQUEST Request)
{
    PHDACODEC Codec = RequestCodec(Request);

    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
        return BasicSupport(Request, VT_BOOL, FALSE, 0, 0, 0);
    if (!ValidChannel(Request))
        return STATUS_INVALID_PARAMETER;
    if (Request->ValueSize < sizeof(BOOL))
        return STATUS_BUFFER_TOO_SMALL;
    if (Request->Verb & KSPROPERTY_TYPE_GET)
    {
        *(PBOOL)Request->Value = Codec->GetMute();
        Request->ValueSize = sizeof(BOOL);
        return STATUS_SUCCESS;
    }
    if (Request->Verb & KSPROPERTY_TYPE_SET)
    {
        Codec->SetMute(*(PBOOL)Request->Value != FALSE);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
CreateTopologyMiniport(PUNKNOWN *Unknown, REFCLSID ClassId, PUNKNOWN OuterUnknown, POOL_TYPE PoolType)
{
    CMiniportTopo *Miniport;

    UNREFERENCED_PARAMETER(ClassId);
    UNREFERENCED_PARAMETER(OuterUnknown);

    Miniport = new (PoolType, HDA_TAG) CMiniportTopo();
    if (!Miniport)
        return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = PUNKNOWN(PMINIPORTTOPOLOGY(Miniport));
    return STATUS_SUCCESS;
}
