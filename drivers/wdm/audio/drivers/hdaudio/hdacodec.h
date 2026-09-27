/*
 * PROJECT:     WinDosDX HD Audio codec driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared declarations
 *
 * A function driver for High Definition Audio codecs on hdaudbus. It finds
 * the codec's analog output pins (speaker, headphone, line out), routes a
 * DAC to each, and plays one PCM stream through the bus driver's DMA
 * engines. PortCls sees a WaveCyclic filter (the stream) and a Topology
 * filter (master volume and mute).
 */

#pragma once

#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>
#include <hdaudio.h>

#define HDA_TAG 'aDHW'

/* The build defines _NEW_DELETE_OPERATORS_, so stdunk.h leaves these out;
   the stdunk library provides them. */
PVOID operator new(size_t Size, POOL_TYPE PoolType, ULONG Tag);

/* ---------------------------------------------------------------------- */
/* Codec verbs and parameters (HD Audio specification 1.0a, section 7)     */
/* ---------------------------------------------------------------------- */

#define VERB_GET_PARAMETER          0xF00
#define VERB_GET_CONNECTION_SELECT  0xF01
#define VERB_SET_CONNECTION_SELECT  0x701
#define VERB_GET_CONNECTION_ENTRY   0xF02
#define VERB_SET_POWER_STATE        0x705
#define VERB_SET_STREAM_CHANNEL     0x706
#define VERB_SET_PIN_CONTROL        0x707
#define VERB_GET_PIN_SENSE          0xF09
#define VERB_SET_EAPD_BTL           0x70C
#define VERB_GET_CONFIG_DEFAULT     0xF1C
#define VERB_SET_GPIO_DATA          0x715
#define VERB_SET_GPIO_ENABLE        0x716
#define VERB_SET_GPIO_DIRECTION     0x717
#define VERB4_SET_CONVERTER_FORMAT  0x2
#define VERB4_SET_AMP_GAIN_MUTE     0x3

#define PARAM_VENDOR_ID             0x00
#define PARAM_NODE_COUNT            0x04
#define PARAM_FUNCTION_TYPE         0x05
#define PARAM_AUDIO_CAPS            0x09
#define PARAM_PCM                   0x0A
#define PARAM_PIN_CAPS              0x0C
#define PARAM_INPUT_AMP_CAPS        0x0D
#define PARAM_CONNECTION_LENGTH     0x0E
#define PARAM_OUTPUT_AMP_CAPS       0x12
#define PARAM_GPIO_COUNT            0x11

/* Widget types (audio widget capabilities, bits 23:20) */
#define WIDGET_OUTPUT               0x0
#define WIDGET_INPUT                0x1
#define WIDGET_MIXER                0x2
#define WIDGET_SELECTOR             0x3
#define WIDGET_PIN                  0x4

/* Audio widget capability bits */
#define WCAP_STEREO                 0x001
#define WCAP_IN_AMP                 0x002
#define WCAP_OUT_AMP                0x004
#define WCAP_AMP_OVERRIDE           0x008
#define WCAP_CONN_LIST              0x100
#define WCAP_DIGITAL                0x200
#define WCAP_POWER_CTRL             0x400

/* Pin capability bits */
#define PINCAP_PRESENCE             0x00000004
#define PINCAP_HP_DRIVE             0x00000008
#define PINCAP_OUTPUT               0x00000010
#define PINCAP_EAPD                 0x00010000

/* Pin widget control */
#define PINCTL_OUT_EN               0x40
#define PINCTL_HP_EN                0x80

/* Default device of a pin (configuration default, bits 23:20) */
#define DEVICE_LINE_OUT             0x0
#define DEVICE_SPEAKER              0x1
#define DEVICE_HP_OUT               0x2

/* Port connectivity (configuration default, bits 31:30) */
#define CONNECT_NONE                0x1

/* PCM sample rate bits (parameter 0x0A) */
#define PCM_RATE_44100              0x20
#define PCM_RATE_48000              0x40

#define HDA_MAX_NODES               128
#define HDA_MAX_CONNECTIONS         32
#define HDA_MAX_PATH                8
#define HDA_MAX_OUTPUTS             4

typedef struct _HDA_WIDGET
{
    ULONG Caps;
    ULONG PinCaps;
    ULONG Config;
    ULONG AmpOutCaps;
    ULONG AmpInCaps;
    UCHAR Type;
    UCHAR ConnectionCount;
    UCHAR Connections[HDA_MAX_CONNECTIONS];
} HDA_WIDGET, *PHDA_WIDGET;

/* A route from a DAC to an output pin: Nodes[0] is the pin, the last node
   is the DAC; Select[i] is the connection index Nodes[i] uses to reach
   Nodes[i + 1]. */
typedef struct _HDA_PATH
{
    UCHAR Count;
    UCHAR Nodes[HDA_MAX_PATH];
    UCHAR Select[HDA_MAX_PATH];
    UCHAR Device;           /* DEVICE_* of the pin */
} HDA_PATH, *PHDA_PATH;

/* ---------------------------------------------------------------------- */
/* The codec (one per adapter device): owns the bus interface               */
/* ---------------------------------------------------------------------- */

DEFINE_GUID(IID_IHdaCodec, 0x5b9f2c61, 0x7e0a, 0x4d3f, 0x9b, 0x21, 0x6a, 0x3e, 0x80, 0x14, 0xc7, 0x55);

class CHdaCodec : public IUnknown
{
public:
    CHdaCodec() : m_Ref(1) {}

    STDMETHODIMP QueryInterface(REFIID Iid, PVOID *Object);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    NTSTATUS Init(PDEVICE_OBJECT DeviceObject);

    /* Stream support for the wave miniport */
    ULONG SupportedRates() const { return m_Rates; }
    NTSTATUS StartStream(ULONG SampleRate, ULONG BufferSize,
                         PVOID *Buffer, PULONG AllocatedSize);
    NTSTATUS RunStream(BOOLEAN Run);
    VOID StopStream();
    ULONG StreamPosition();

    /* Master volume (-96..0 dB in 1/65536 dB) and mute, for the topology */
    VOID SetVolume(LONG Level);
    LONG GetVolume() const { return m_Volume; }
    VOID SetMute(BOOLEAN Mute);
    BOOLEAN GetMute() const { return m_Mute; }
    VOID GetVolumeRange(PLONG Minimum, PLONG Maximum, PULONG Step);

private:
    ~CHdaCodec();

    NTSTATUS Verb(ULONG Node, ULONG VerbId, ULONG Data, PULONG Response = NULL);
    NTSTATUS Verb4(ULONG Node, ULONG VerbId, ULONG Payload);
    ULONG Parameter(ULONG Node, ULONG Param);
    VOID ReadWidget(UCHAR Node);
    BOOLEAN FindPath(UCHAR Pin, PHDA_PATH Path, UCHAR AvoidDac);
    VOID SetupPath(PHDA_PATH Path);
    VOID SetAmp(UCHAR Node, BOOLEAN Output, UCHAR Index, BOOLEAN Mute, ULONG Gain);
    VOID ApplyVolume();

    LONG m_Ref;
    PDEVICE_OBJECT m_DeviceObject;
    HDAUDIO_BUS_INTERFACE m_Bus;
    BOOLEAN m_HaveBus;
    UCHAR m_Codec;
    UCHAR m_Afg;
    UCHAR m_FirstNode;
    UCHAR m_NodeCount;
    ULONG m_AfgAmpOutCaps;
    ULONG m_AfgAmpInCaps;
    ULONG m_Rates;
    HDA_WIDGET m_Widgets[HDA_MAX_NODES];

    HDA_PATH m_Paths[HDA_MAX_OUTPUTS];
    UCHAR m_PathCount;

    HANDLE m_DmaEngine;
    PMDL m_BufferMdl;
    SIZE_T m_BufferSize;
    UCHAR m_StreamId;
    PULONG m_Position;
    HDAUDIO_CONVERTER_FORMAT m_Format;

    LONG m_Volume;
    BOOLEAN m_Mute;
};

typedef CHdaCodec *PHDACODEC;

/* ---------------------------------------------------------------------- */
/* Miniports                                                                */
/* ---------------------------------------------------------------------- */

/* Wave filter: pin 0 is the PCM render sink, pin 1 the analog bridge out. */
#define WAVE_PIN_RENDER             0
#define WAVE_PIN_BRIDGE             1

/* Topology filter: pin 0 from the wave filter, pin 1 the speaker/line out. */
#define TOPO_PIN_WAVEIN             0
#define TOPO_PIN_LINEOUT            1
#define TOPO_NODE_VOLUME            0
#define TOPO_NODE_MUTE              1

NTSTATUS CreateWaveMiniport(PUNKNOWN *Unknown, REFCLSID ClassId,
                            PUNKNOWN OuterUnknown, POOL_TYPE PoolType);
NTSTATUS CreateTopologyMiniport(PUNKNOWN *Unknown, REFCLSID ClassId,
                                PUNKNOWN OuterUnknown, POOL_TYPE PoolType);
