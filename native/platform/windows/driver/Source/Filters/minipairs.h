/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minipairs.h

Abstract:

    Local audio endpoint filter definitions. 
--*/

#ifndef _SIMPLEAUDIOSAMPLE_MINIPAIRS_H_
#define _SIMPLEAUDIOSAMPLE_MINIPAIRS_H_

#include "speakertopo.h"
#include "speakertoptable.h"
#include "speakerwavtable.h"

#include "micarraytopo.h"
#include "micarray1toptable.h"
#include "micarraywavtable.h"

NTSTATUS
CreateMiniportWaveRTSimpleAudioSample(_Out_ PUNKNOWN*, _In_ REFCLSID, _In_opt_ PUNKNOWN,
                                      _In_ POOL_FLAGS, _In_ PUNKNOWN, _In_opt_ PVOID,
                                      _In_ PENDPOINT_MINIPAIR);

NTSTATUS
CreateMiniportTopologySimpleAudioSample(_Out_ PUNKNOWN*, _In_ REFCLSID, _In_opt_ PUNKNOWN,
                                        _In_ POOL_FLAGS, _In_ PUNKNOWN, _In_opt_ PVOID,
                                        _In_ PENDPOINT_MINIPAIR);

//
// Render miniports.
//

/*********************************************************************
* Topology/Wave bridge connection for speaker (internal)             *
*                                                                    *
*              +------+                +------+                      *
*              | Wave |                | Topo |                      *
*              |      |                |      |                      *
* System   --->|0    1|--------------->|0    1|---> Line Out         *
*              |      |                |      |                      *
*              +------+                +------+                      *
*********************************************************************/
static PHYSICALCONNECTIONTABLE SpeakerTopologyPhysicalConnections[] = {
    {KSPIN_TOPO_WAVEOUT_SOURCE, // TopologyIn
     KSPIN_WAVE_RENDER3_SOURCE, // WaveOut
     CONNECTIONTYPE_WAVE_OUTPUT}};

// Declare a supported 4 ms processing quantum for both public modes. The
// absolute 2 ms transport floor is separate from each mode's processing period.
// Supplying explicit mode constraints lets Windows derive shared engine periods
// instead of falling back to its legacy 10 ms-only processing quantum.
static struct {
    KSAUDIO_PACKETSIZE_CONSTRAINTS2 transport;
    KSAUDIO_PACKETSIZE_PROCESSINGMODE_CONSTRAINT additional[1];
} LoopZPacketConstraints = {
    {2 * HNSTIME_PER_MILLISECOND, FILE_BYTE_ALIGNMENT, 0, 2,
     {{STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT, 0, 4 * HNSTIME_PER_MILLISECOND}}},
    {{STATIC_AUDIO_SIGNALPROCESSINGMODE_RAW, 0, 4 * HNSTIME_PER_MILLISECOND}}};
static const SIMPLEAUDIOSAMPLE_DEVPROPERTY LoopZWaveProperties[] = {
    {&DEVPKEY_KsAudio_PacketSize_Constraints2, DEVPROP_TYPE_BINARY,
     sizeof(LoopZPacketConstraints), &LoopZPacketConstraints}};

static ENDPOINT_MINIPAIR SpeakerMiniports = {
    eSpeakerDevice,
    L"TopologySpeaker", // make sure this or the template name matches with KSNAME_TopologySpeaker in the inf's [Strings] section
    NULL,               // optional template name
    CreateMiniportTopologySimpleAudioSample,
    &SpeakerTopoMiniportFilterDescriptor,
    0,
    NULL,           // Interface properties
    L"WaveSpeaker", // make sure this or the template name matches with KSNAME_WaveSpeaker in the inf's [Strings] section
    NULL,           // optional template name
    CreateMiniportWaveRTSimpleAudioSample,
    &SpeakerWaveMiniportFilterDescriptor,
    SIZEOF_ARRAY(LoopZWaveProperties),
    LoopZWaveProperties,
    SPEAKER_DEVICE_MAX_CHANNELS,
    SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes),
    SpeakerTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpeakerTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

//
// Capture miniports.
//

/*********************************************************************
* Topology/Wave bridge connection for mic array  1 (front)           *
*                                                                    *
*              +------+    +------+                                  *
*              | Topo |    | Wave |                                  *
*              |      |    |      |                                  *
*  Mic in  --->|0    1|===>|0    1|---> Capture Host Pin             *
*              |      |    |      |                                  *
*              +------+    +------+                                  *
*********************************************************************/
static PHYSICALCONNECTIONTABLE MicArray1TopologyPhysicalConnections[] = {
    {KSPIN_TOPO_BRIDGE, // TopologyOut
     KSPIN_WAVE_BRIDGE, // WaveIn
     CONNECTIONTYPE_TOPOLOGY_OUTPUT}};

static ENDPOINT_MINIPAIR MicArray1Miniports = {
    eMicArrayDevice1,
    L"TopologyMicArray1", // make sure this or the template name matches with KSNAME_TopologyMicArray1 in the inf's [Strings] section
    NULL,                 // optional template name
    CreateMicArrayMiniportTopology,
    &MicArray1TopoMiniportFilterDescriptor,
    0,
    NULL,             // Interface properties
    L"WaveMicArray1", // make sure this or the tempalte name matches with KSNAME_WaveMicArray1 in the inf's [Strings] section
    NULL,             // optional template name
    CreateMiniportWaveRTSimpleAudioSample,
    &MicArrayWaveMiniportFilterDescriptor,
    SIZEOF_ARRAY(LoopZWaveProperties),
    LoopZWaveProperties,
    MICARRAY_DEVICE_MAX_CHANNELS,
    MicArrayPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(MicArrayPinDeviceFormatsAndModes),
    MicArray1TopologyPhysicalConnections,
    SIZEOF_ARRAY(MicArray1TopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

//=============================================================================
//
// Render miniport pairs. NOTE: the split of render and capture is arbitrary and
// unnessary, this array could contain capture endpoints.
//
static PENDPOINT_MINIPAIR g_RenderEndpoints[] = {
    &SpeakerMiniports,
};

#define g_cRenderEndpoints (SIZEOF_ARRAY(g_RenderEndpoints))

//=============================================================================
//
// Capture miniport pairs. NOTE: the split of render and capture is arbitrary and
// unnessary, this array could contain render endpoints.
//
static PENDPOINT_MINIPAIR g_CaptureEndpoints[] = {
    &MicArray1Miniports,
};

#define g_cCaptureEndpoints (SIZEOF_ARRAY(g_CaptureEndpoints))

//=============================================================================
//
// Total miniports = # endpoints * 2 (topology + wave).
//
#define g_MaxMiniports ((g_cRenderEndpoints + g_cCaptureEndpoints) * 2)

#endif // _SIMPLEAUDIOSAMPLE_MINIPAIRS_H_
