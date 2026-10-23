#include <ntifs.h>
/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    common.cpp

Abstract:

    Implementation of the AdapterCommon class. 
--*/

#pragma warning(disable : 4127)

#include <initguid.h>
#include <devpkey.h>
#include "definitions.h"
#include "hw.h"

#include "endpoints.h"
#include "../../transport_protocol.hpp"
#include "../../packed_samples.hpp"
#include "../../frame_clock.hpp"
#include "../../oem_format.hpp"
#include "../../endpoint_reference.hpp"
#include "../../adapter_configuration.hpp"

//=============================================================================
// Classes
//=============================================================================

///////////////////////////////////////////////////////////////////////////////
// CAdapterCommon
//
class CAdapterCommon : public IAdapterCommon, public IAdapterPowerManagement, public CUnknown {
  private:
    KSPIN_LOCK m_BridgeLock;
    // PortCls retains Name pointers. These buffers live with the adapter and
    // its referenced port/miniport objects, never in a registration stack frame.
    WCHAR m_EndpointReferences[4][192]{};
    ULONG m_EndpointReferenceCount{};
    loopz_driver::AudioFormat m_AudioFormat{};
    BOOLEAN m_ExposeCaptureEndpoint = TRUE;
    PBYTE m_AudioStorage = nullptr;
    loopz_driver::FloatDuplexBridge m_Bridge;
    // Adapter lives in nonpaged pool; never put maximum packets on a DPC stack.
    loopz_driver::FloatDuplexBridge::Packet m_Conversion;
    unsigned m_TransportVersion = 0;
    PEPROCESS m_TransportOwner = nullptr;
    PSERVICEGROUP m_pServiceGroupWave;
    PDEVICE_OBJECT m_pDeviceObject;
    PDEVICE_OBJECT m_pPhysicalDeviceObject;
    WDFDEVICE m_WdfDevice; // Wdf device.
    DEVICE_POWER_STATE m_PowerState;

    PCSimpleAudioSampleHW m_pHW; // Virtual Simple Audio Sample HW object
    PPORTCLSETWHELPER m_pPortClsEtwHelper;

    DWORD m_dwIdleRequests;

  public:
    BOOLEAN ExposesCaptureEndpoint() override {
        return m_ExposeCaptureEndpoint;
    }
    void BridgeFormat(PULONG rate, PULONG channels) override {
        *rate = m_AudioFormat.sampleRate;
        *channels = m_AudioFormat.channels;
    }
    NTSTATUS LoadAudioFormat();
    NTSTATUS PublishAudioFormat(PUNICODE_STRING link);
    void BridgeCapture(BOOLEAN active) override {
        KIRQL irql;
        KeAcquireSpinLock(&m_BridgeLock, &irql);
        m_Bridge.capture(active != FALSE);
        KeReleaseSpinLock(&m_BridgeLock, irql);
    }
    void BridgeReset(BOOLEAN running) override {
        KIRQL irql;
        KeAcquireSpinLock(&m_BridgeLock, &irql);
        m_Bridge.playback(running != FALSE);
        KeReleaseSpinLock(&m_BridgeLock, irql);
    }
    void BridgeWrite(PBYTE bytes, ULONG frames, ULONGLONG anchorTime, ULONGLONG firstFrame,
                     ULONG sampleBytes) override {
        KIRQL irql;
        KeAcquireSpinLock(&m_BridgeLock, &irql);
        const auto now = KeQueryInterruptTime();
        for (ULONG offset = 0; offset < frames;) {
            const ULONG count = min(frames - offset, loopz_driver::FloatDuplexBridge::packetFrames);
            const ULONG samples = count * m_AudioFormat.channels;
            loopz_driver::convertPackedSamples(
                bytes + offset * m_AudioFormat.channels * sampleBytes, sampleBytes,
                samples * sampleBytes, m_Conversion.samples, 4, sizeof(m_Conversion.samples),
                samples);
            m_Bridge.pushPlayback(m_Conversion.samples, count,
                                  anchorTime + loopz_driver::FrameClock::timeForFrames(
                                                   firstFrame + offset, m_AudioFormat.sampleRate),
                                  now);
            offset += count;
        }
        KeReleaseSpinLock(&m_BridgeLock, irql);
    }
    void BridgeRead(PBYTE bytes, ULONG frames, ULONG sampleBytes) override {
        KIRQL irql;
        KeAcquireSpinLock(&m_BridgeLock, &irql);
        const auto now = KeQueryInterruptTime();
        for (ULONG offset = 0; offset < frames;) {
            const ULONG count = min(frames - offset, loopz_driver::FloatDuplexBridge::packetFrames);
            const ULONG samples = count * m_AudioFormat.channels;
            m_Bridge.readRecording(m_Conversion.samples, count, now);
            loopz_driver::convertPackedSamples(m_Conversion.samples, 4, samples * 4,
                                               bytes +
                                                   offset * m_AudioFormat.channels * sampleBytes,
                                               sampleBytes, samples * sampleBytes, samples);
            offset += count;
        }
        KeReleaseSpinLock(&m_BridgeLock, irql);
    }
    NTSTATUS TransportProperty(PPCPROPERTY_REQUEST request) override;
    bool AuthorizeTransport(PPCPROPERTY_REQUEST request);
    //=====================================================================
    // Default CUnknown
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CAdapterCommon);
    ~CAdapterCommon();

    //=====================================================================
    // Default IAdapterPowerManagement
    IMP_IAdapterPowerManagement;

    //=====================================================================
    // IAdapterCommon methods

    STDMETHODIMP_(NTSTATUS) Init(_In_ PDEVICE_OBJECT DeviceObject);

    STDMETHODIMP_(PDEVICE_OBJECT) GetDeviceObject(void);

    STDMETHODIMP_(PDEVICE_OBJECT) GetPhysicalDeviceObject(void);

    STDMETHODIMP_(WDFDEVICE) GetWdfDevice(void);

    STDMETHODIMP_(void) SetWaveServiceGroup(_In_ PSERVICEGROUP ServiceGroup);

    STDMETHODIMP_(BOOL) bDevSpecificRead();

    STDMETHODIMP_(void) bDevSpecificWrite(_In_ BOOL bDevSpecific);
    STDMETHODIMP_(INT) iDevSpecificRead();

    STDMETHODIMP_(void) iDevSpecificWrite(_In_ INT iDevSpecific);
    STDMETHODIMP_(UINT) uiDevSpecificRead();

    STDMETHODIMP_(void) uiDevSpecificWrite(_In_ UINT uiDevSpecific);

    STDMETHODIMP_(BOOL) MixerMuteRead(_In_ ULONG Index, _In_ ULONG Channel);

    STDMETHODIMP_(void) MixerMuteWrite(_In_ ULONG Index, _In_ ULONG Channel, _In_ BOOL Value);

    STDMETHODIMP_(ULONG) MixerMuxRead(void);

    STDMETHODIMP_(void) MixerMuxWrite(_In_ ULONG Index);

    STDMETHODIMP_(void) MixerReset(void);

    STDMETHODIMP_(LONG) MixerVolumeRead(_In_ ULONG Index, _In_ ULONG Channel);

    STDMETHODIMP_(void) MixerVolumeWrite(_In_ ULONG Index, _In_ ULONG Channel, _In_ LONG Value);

    STDMETHODIMP_(LONG) MixerPeakMeterRead(_In_ ULONG Index, _In_ ULONG Channel);

    STDMETHODIMP_(NTSTATUS)
    WriteEtwEvent(_In_ EPcMiniportEngineEvent miniportEventType, _In_ ULONGLONG ullData1,
                  _In_ ULONGLONG ullData2, _In_ ULONGLONG ullData3, _In_ ULONGLONG ullData4);

    STDMETHODIMP_(VOID) SetEtwHelper(PPORTCLSETWHELPER _pPortClsEtwHelper);

    STDMETHODIMP_(NTSTATUS)
    InstallSubdevice(_In_opt_ PIRP Irp, _In_ PWSTR Name, _In_opt_ PWSTR TemplateName,
                     _In_ REFGUID PortClassId, _In_ REFGUID MiniportClassId,
                     _In_opt_ PFNCREATEMINIPORT MiniportCreate, _In_ ULONG cPropertyCount,
                     _In_reads_opt_(cPropertyCount)
                         const SIMPLEAUDIOSAMPLE_DEVPROPERTY* pProperties,
                     _In_opt_ PVOID DeviceContext, _In_ PENDPOINT_MINIPAIR MiniportPair,
                     _In_opt_ PRESOURCELIST ResourceList, _In_ REFGUID PortInterfaceId,
                     _Out_opt_ PUNKNOWN* OutPortInterface, _Out_opt_ PUNKNOWN* OutPortUnknown,
                     _Out_opt_ PUNKNOWN* OutMiniportUnknown);

    STDMETHODIMP_(NTSTATUS) UnregisterSubdevice(_In_opt_ PUNKNOWN UnknownPort);

    STDMETHODIMP_(NTSTATUS)
    ConnectTopologies(_In_ PUNKNOWN UnknownTopology, _In_ PUNKNOWN UnknownWave,
                      _In_ PHYSICALCONNECTIONTABLE* PhysicalConnections,
                      _In_ ULONG PhysicalConnectionCount);

    STDMETHODIMP_(NTSTATUS)
    DisconnectTopologies(_In_ PUNKNOWN UnknownTopology, _In_ PUNKNOWN UnknownWave,
                         _In_ PHYSICALCONNECTIONTABLE* PhysicalConnections,
                         _In_ ULONG PhysicalConnectionCount);

    STDMETHODIMP_(NTSTATUS)
    InstallEndpointFilters(_In_opt_ PIRP Irp, _In_ PENDPOINT_MINIPAIR MiniportPair,
                           _In_opt_ PVOID DeviceContext, _Out_opt_ PUNKNOWN* UnknownTopology,
                           _Out_opt_ PUNKNOWN* UnknownWave,
                           _Out_opt_ PUNKNOWN* UnknownMiniportTopology,
                           _Out_opt_ PUNKNOWN* UnknownMiniportWave);

    STDMETHODIMP_(NTSTATUS)
    RemoveEndpointFilters(_In_ PENDPOINT_MINIPAIR MiniportPair, _In_opt_ PUNKNOWN UnknownTopology,
                          _In_opt_ PUNKNOWN UnknownWave);

    STDMETHODIMP_(NTSTATUS)
    GetFilters(_In_ PENDPOINT_MINIPAIR MiniportPair, _Out_opt_ PUNKNOWN* UnknownTopologyPort,
               _Out_opt_ PUNKNOWN* UnknownTopologyMiniport, _Out_opt_ PUNKNOWN* UnknownWavePort,
               _Out_opt_ PUNKNOWN* UnknownWaveMiniport);

    STDMETHODIMP_(NTSTATUS)
    SetIdlePowerManagement(_In_ PENDPOINT_MINIPAIR MiniportPair, _In_ BOOL bEnabled);

    STDMETHODIMP_(VOID) Cleanup();

    //=====================================================================
    // friends
    friend NTSTATUS NewAdapterCommon(_Out_ PUNKNOWN* Unknown, _In_ REFCLSID,
                                     _In_opt_ PUNKNOWN UnknownOuter, _In_ POOL_FLAGS PoolFlags);

  private:
    LIST_ENTRY m_SubdeviceCache;

    NTSTATUS GetCachedSubdevice(_In_ PWSTR Name, _Out_opt_ PUNKNOWN* OutUnknownPort,
                                _Out_opt_ PUNKNOWN* OutUnknownMiniport);

    NTSTATUS CacheSubdevice(_In_ PWSTR Name, _In_ PUNKNOWN UnknownPort,
                            _In_ PUNKNOWN UnknownMiniport);

    NTSTATUS RemoveCachedSubdevice(_In_ PWSTR Name);

    VOID EmptySubdeviceCache();

    NTSTATUS CreateAudioInterfaceWithProperties(
        _In_ PCWSTR ReferenceString, _In_opt_ PCWSTR TemplateReferenceString,
        _In_ ULONG cPropertyCount,
        _In_reads_opt_(cPropertyCount) const SIMPLEAUDIOSAMPLE_DEVPROPERTY* pProperties,
        _Out_ _At_(AudioSymbolicLinkName->Buffer, __drv_allocatesMem(Mem))
            PUNICODE_STRING AudioSymbolicLinkName,
        _In_opt_ const GUID* InterfaceCategory = nullptr);

    NTSTATUS MigrateDeviceInterfaceTemplateParameters(_In_ PUNICODE_STRING SymbolicLinkName,
                                                      _In_opt_ PCWSTR TemplateReferenceString);
};

typedef struct _MINIPAIR_UNKNOWN {
    LIST_ENTRY ListEntry;
    WCHAR Name[MAX_PATH];
    PUNKNOWN PortInterface;
    PUNKNOWN MiniportInterface;
    PADAPTERPOWERMANAGEMENT PowerInterface;
} MINIPAIR_UNKNOWN;

#define MAX_DEVICE_REG_KEY_LENGTH 0x100

//
// Used to implement the singleton pattern.
//

//-----------------------------------------------------------------------------
// Functions
//-----------------------------------------------------------------------------

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS SimpleAudioSampleIoSetDeviceInterfacePropertyDataMultiple(
    _In_ PUNICODE_STRING SymbolicLinkName, _In_ ULONG cPropertyCount,
    _In_reads_opt_(cPropertyCount) const SIMPLEAUDIOSAMPLE_DEVPROPERTY* pProperties) {
    NTSTATUS ntStatus;

    PAGED_CODE();

    if (pProperties) {
        for (ULONG i = 0; i < cPropertyCount; i++) {
            ntStatus = IoSetDeviceInterfacePropertyData(
                SymbolicLinkName, pProperties[i].PropertyKey, LOCALE_NEUTRAL,
                PLUGPLAY_PROPERTY_PERSISTENT, pProperties[i].Type, pProperties[i].BufferSize,
                pProperties[i].Buffer);

            if (!NT_SUCCESS(ntStatus)) {
                return ntStatus;
            }
        }
    }

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
NewAdapterCommon(_Out_ PUNKNOWN* Unknown, _In_ REFCLSID, _In_opt_ PUNKNOWN UnknownOuter,
                 _In_ POOL_FLAGS PoolFlags)
/*++

Routine Description:

  Creates a new CAdapterCommon

Arguments:

  Unknown - 

  UnknownOuter -

  PoolType

Return Value:

  NT status code.

--*/
{
    PAGED_CODE();

    ASSERT(Unknown);

    NTSTATUS ntStatus;

    // Each root adapter owns its own FIFO and endpoint pair.
    //
    // Allocate an adapter object.
    //
    CAdapterCommon* p = new (PoolFlags, MINADAPTER_POOLTAG) CAdapterCommon(UnknownOuter);
    if (p == NULL) {
        ntStatus = STATUS_INSUFFICIENT_RESOURCES;
        DPF(D_ERROR, ("NewAdapterCommon failed, 0x%x", ntStatus));
        goto Done;
    }

    //
    // Success.
    //
    *Unknown = PUNKNOWN((PADAPTERCOMMON)(p));
    (*Unknown)->AddRef();
    ntStatus = STATUS_SUCCESS;

Done:
    return ntStatus;
} // NewAdapterCommon

// Private properties execute at PASSIVE_LEVEL. Owner verification and registry
// reads occur outside the audio lock; only bounded queue operations hold it.
#pragma code_seg("PAGE")
bool CAdapterCommon::AuthorizeTransport(PPCPROPERTY_REQUEST request) {
    PAGED_CODE();
    if (!request->Irp || IoGetRequestorProcess(request->Irp) != PsGetCurrentProcess())
        return false;
    HANDLE key{};
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(m_pPhysicalDeviceObject, PLUGPLAY_REGKEY_DEVICE,
                                            KEY_QUERY_VALUE, &key)))
        return false;
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"LoopZOwnerSid");
    alignas(8) UCHAR storage[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + SECURITY_MAX_SID_SIZE]{};
    auto* value = reinterpret_cast<PKEY_VALUE_PARTIAL_INFORMATION>(storage);
    ULONG length{};
    const auto status =
        ZwQueryValueKey(key, &name, KeyValuePartialInformation, value, sizeof(storage), &length);
    ZwClose(key);
    if (!NT_SUCCESS(status) || value->Type != REG_BINARY || value->DataLength < 8 ||
        value->DataLength > SECURITY_MAX_SID_SIZE)
        return false;
    auto* sid = reinterpret_cast<SID*>(value->Data);
    if (sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES ||
        8u + sid->SubAuthorityCount * sizeof(ULONG) != value->DataLength || !RtlValidSid(sid))
        return false;
    SECURITY_SUBJECT_CONTEXT subject{};
    SeCaptureSubjectContext(&subject);
    PTOKEN_USER user{};
    const auto queried = SeQueryInformationToken(SeQuerySubjectContextToken(&subject), TokenUser,
                                                 reinterpret_cast<PVOID*>(&user));
    const bool allowed = NT_SUCCESS(queried) && user && RtlEqualSid(sid, user->User.Sid);
    if (user)
        ExFreePool(user);
    SeReleaseSubjectContext(&subject);
    return allowed;
}

#pragma code_seg()
NTSTATUS CAdapterCommon::TransportProperty(PPCPROPERTY_REQUEST request) {
    using namespace loopz_driver;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !request || !request->PropertyItem ||
        request->MinorTarget || request->Node != ULONG(-1))
        return STATUS_INVALID_PARAMETER;
    const auto property = static_cast<loopz_driver::TransportProperty>(request->PropertyItem->Id);
    ULONG bytes{}, verb = KSPROPERTY_TYPE_GET;
    switch (property) {
    case loopz_driver::TransportProperty::Capabilities:
        bytes = sizeof(TransportCapabilities);
        break;
    case loopz_driver::TransportProperty::Connect:
        bytes = sizeof(TransportConnection);
        break;
    case loopz_driver::TransportProperty::Playback:
        bytes = sizeof(DuplexBridge::Packet);
        break;
    case loopz_driver::TransportProperty::Diagnostics:
        bytes = sizeof(DuplexBridge::Diagnostics);
        break;
    case loopz_driver::TransportProperty::Processed:
        bytes = sizeof(TransportProcessed);
        verb = KSPROPERTY_TYPE_SET;
        break;
    case loopz_driver::TransportProperty::Disconnect:
        verb = KSPROPERTY_TYPE_SET;
        break;
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (request->Verb != verb || request->InstanceSize != sizeof(TransportInstance) ||
        !request->Instance)
        return STATUS_INVALID_PARAMETER;
    TransportInstance instance{};
    RtlCopyMemory(&instance, request->Instance, sizeof(instance));
    if ((instance.version != transportVersion && instance.version != floatTransportVersion) ||
        instance.reserved ||
        ((property == loopz_driver::TransportProperty::Connect ||
          property == loopz_driver::TransportProperty::Capabilities) &&
         instance.generation))
        return STATUS_INVALID_PARAMETER;
    if (!request->ValueSize && bytes && verb == KSPROPERTY_TYPE_GET) {
        request->ValueSize = bytes;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (request->ValueSize != bytes || (bytes && !request->Value))
        return STATUS_INVALID_BUFFER_SIZE;
    if (property == loopz_driver::TransportProperty::Capabilities) {
        TransportCapabilities caps{};
        caps.version = instance.version;
        caps.bitsPerSample = instance.version == floatTransportVersion ? 32 : 16;
        caps.maxPacketFrames = instance.version == floatTransportVersion
                                   ? FloatDuplexBridge::packetFrames
                                   : DuplexBridge::packetFrames;
        caps.sampleRate = m_AudioFormat.sampleRate;
        caps.channels = m_AudioFormat.channels;
        RtlCopyMemory(request->Value, &caps, sizeof(caps));
        return STATUS_SUCCESS;
    }
    if (!request->Irp)
        return STATUS_ACCESS_DENIED;
    PEPROCESS caller = IoGetRequestorProcess(request->Irp);
    if (!caller ||
        (property == loopz_driver::TransportProperty::Connect && !AuthorizeTransport(request)))
        return STATUS_ACCESS_DENIED;
    // Never touch caller buffers or pageable data while holding the audio lock.
    struct Staging {
        PVOID value;
        ~Staging() {
            if (value)
                ExFreePool(value);
        }
    } staging{bytes ? ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, 'tZLL') : nullptr};
    if (bytes && !staging.value)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (property == loopz_driver::TransportProperty::Processed) {
        RtlCopyMemory(staging.value, request->Value, bytes);
        auto* data = static_cast<const TransportProcessed*>(staging.value);
        if (data->reserved || !data->frames ||
            data->frames > (instance.version == floatTransportVersion
                                ? FloatDuplexBridge::packetFrames
                                : DuplexBridge::packetFrames))
            return STATUS_INVALID_PARAMETER;
    }
    PEPROCESS previousOwner = nullptr;
    NTSTATUS result = STATUS_SUCCESS;
    KIRQL irql;
    KeAcquireSpinLock(&m_BridgeLock, &irql);
    const auto now = KeQueryInterruptTime();
    if (property == loopz_driver::TransportProperty::Connect) {
        const auto generation = m_Bridge.connect(now);
        if (!generation)
            result = STATUS_DEVICE_BUSY;
        else {
            ObReferenceObject(caller);
            previousOwner = m_TransportOwner;
            m_TransportOwner = caller;
            m_TransportVersion = instance.version;
            static_cast<TransportConnection*>(staging.value)->generation = generation;
        }
    } else if (caller != m_TransportOwner || instance.version != m_TransportVersion ||
               !m_Bridge.heartbeat(instance.generation, now)) {
        result = STATUS_ACCESS_DENIED;
    } else if (property == loopz_driver::TransportProperty::Disconnect) {
        m_Bridge.disconnect(instance.generation);
        previousOwner = m_TransportOwner;
        m_TransportOwner = nullptr;
        m_TransportVersion = 0;
    } else if (property == loopz_driver::TransportProperty::Playback) {
        if (instance.version == floatTransportVersion) {
            m_Bridge.takePlayback(instance.generation, now,
                                  *static_cast<FloatDuplexBridge::Packet*>(staging.value));
        } else {
            m_Bridge.takePlayback(instance.generation, now, m_Conversion);
            auto* packet = static_cast<DuplexBridge::Packet*>(staging.value);
            RtlZeroMemory(packet, sizeof(*packet));
            packet->time = m_Conversion.time;
            packet->sequence = m_Conversion.sequence;
            packet->frames = m_Conversion.frames;
            packet->discontinuity = m_Conversion.discontinuity;
            convertPackedSamples(m_Conversion.samples, 4, sizeof(m_Conversion.samples),
                                 packet->samples, 2, sizeof(packet->samples),
                                 packet->frames * m_AudioFormat.channels);
        }
    } else if (property == loopz_driver::TransportProperty::Diagnostics) {
        *static_cast<DuplexBridge::Diagnostics*>(staging.value) = m_Bridge.diagnostics();
        static_cast<DuplexBridge::Diagnostics*>(staging.value)->sampleTime = now;
    } else if (property == loopz_driver::TransportProperty::Processed) {
        auto* data = static_cast<const TransportProcessed*>(staging.value);
        const unsigned sampleBytes = instance.version == floatTransportVersion ? 4 : 2;
        for (unsigned offset = 0; offset < data->frames;) {
            const unsigned count = min(data->frames - offset, FloatDuplexBridge::packetFrames);
            const unsigned samples = count * m_AudioFormat.channels;
            convertPackedSamples(data->samples + offset * m_AudioFormat.channels * sampleBytes,
                                 sampleBytes, samples * sampleBytes, m_Conversion.samples, 4,
                                 sizeof(m_Conversion.samples), samples);
            if (!m_Bridge.pushProcessed(instance.generation, now, m_Conversion.samples, count)) {
                result = STATUS_DEVICE_NOT_READY;
                break;
            }
            offset += count;
        }
    }
    KeReleaseSpinLock(&m_BridgeLock, irql);
    if (previousOwner)
        ObDereferenceObject(previousOwner);
    if (NT_SUCCESS(result)) {
        if (bytes && verb == KSPROPERTY_TYPE_GET)
            RtlCopyMemory(request->Value, staging.value, bytes);
        request->ValueSize = bytes;
    }
    return result;
}

//=============================================================================
#pragma code_seg("PAGE")
CAdapterCommon::~CAdapterCommon(void)
/*++

Routine Description:

  Destructor for CAdapterCommon.

Arguments:

Return Value:

  void

--*/
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::~CAdapterCommon]"));

    if (m_pHW) {
        delete m_pHW;
        m_pHW = NULL;
    }

    SAFE_RELEASE(m_pPortClsEtwHelper);
    SAFE_RELEASE(m_pServiceGroupWave);
    if (m_TransportOwner) {
        ObDereferenceObject(m_TransportOwner);
        m_TransportOwner = nullptr;
    }

    if (m_WdfDevice) {
        WdfObjectDelete(m_WdfDevice);
        m_WdfDevice = NULL;
    }

    if (m_AudioStorage) {
        ExFreePoolWithTag(m_AudioStorage, 'fZLL');
        m_AudioStorage = nullptr;
    }
} // ~CAdapterCommon

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(PDEVICE_OBJECT)
CAdapterCommon::GetDeviceObject(void)
/*++

Routine Description:

  Returns the deviceobject

Arguments:

Return Value:

  PDEVICE_OBJECT

--*/
{
    PAGED_CODE();

    return m_pDeviceObject;
} // GetDeviceObject

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(PDEVICE_OBJECT)
CAdapterCommon::GetPhysicalDeviceObject(void)
/*++

Routine Description:

  Returns the PDO.

Arguments:

Return Value:

  PDEVICE_OBJECT

--*/
{
    PAGED_CODE();

    return m_pPhysicalDeviceObject;
} // GetPhysicalDeviceObject

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(WDFDEVICE)
CAdapterCommon::GetWdfDevice(void)
/*++

Routine Description:

  Returns the associated WDF miniport device. Note that this is NOT an audio
  miniport. The WDF miniport device is the WDF device associated with the
  adapter.

Arguments:

Return Value:

  WDFDEVICE

--*/
{
    PAGED_CODE();

    return m_WdfDevice;
} // GetWdfDevice

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CAdapterCommon::PublishAudioFormat(PUNICODE_STRING link) {
    PAGED_CODE();
    // Publish the manufacturer's current default before enabling this interface.
    // MMDevice properties are owned by Windows; only our driver interface's
    // documented EP metadata is written here.
    loopz_driver::OemFormatProperty property;
    if (!property.build(m_AudioFormat, 4))
        return STATUS_INVALID_PARAMETER;
    struct Key {
        HANDLE value{};
        ~Key() {
            if (value)
                ZwClose(value);
        }
    } interfaceKey, endpointsKey, endpointKey;
    NTSTATUS status = IoOpenDeviceInterfaceRegistryKey(link, GENERIC_WRITE, &interfaceKey.value);
    if (!NT_SUCCESS(status))
        return status;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    RtlInitUnicodeString(&name, L"EP");
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               interfaceKey.value, nullptr);
    status = ZwCreateKey(&endpointsKey.value, KEY_CREATE_SUB_KEY, &attributes, 0, nullptr,
                         REG_OPTION_NON_VOLATILE, nullptr);
    if (!NT_SUCCESS(status))
        return status;
    RtlInitUnicodeString(&name, L"0");
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               endpointsKey.value, nullptr);
    status = ZwCreateKey(&endpointKey.value, KEY_SET_VALUE, &attributes, 0, nullptr,
                         REG_OPTION_NON_VOLATILE, nullptr);
    if (!NT_SUCCESS(status))
        return status;
    RtlInitUnicodeString(&name, L"{E4870E26-3CC5-4CD2-BA46-CA0A9A70ED04},3");
    status = ZwSetValueKey(endpointKey.value, &name, 0, REG_BINARY, property.data,
                           sizeof(property.data));
    if (!NT_SUCCESS(status))
        return status;
    return status;
}

NTSTATUS CAdapterCommon::LoadAudioFormat() {
    PAGED_CODE();
    HANDLE key{};
    NTSTATUS status = IoOpenDeviceRegistryKey(m_pPhysicalDeviceObject, PLUGPLAY_REGKEY_DEVICE,
                                              KEY_QUERY_VALUE, &key);
    if (!NT_SUCCESS(status))
        return status;
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"LoopZAudioFormat");
    alignas(ULONG) UCHAR buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) +
                                sizeof(loopz_driver::AudioFormatRecord)]{};
    ULONG bytes{};
    status =
        ZwQueryValueKey(key, &name, KeyValuePartialInformation, buffer, sizeof(buffer), &bytes);
    ZwClose(key);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
        m_AudioFormat = {}; // Existing stereo roots predate the format record.
        m_ExposeCaptureEndpoint = TRUE;
    } else {
        if (!NT_SUCCESS(status))
            return status;
        auto* value = reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION*>(buffer);
        if (value->Type != REG_BINARY ||
            value->DataLength != sizeof(loopz_driver::AudioFormatRecord))
            return STATUS_INVALID_PARAMETER;
        loopz_driver::AudioFormatRecord record;
        RtlCopyMemory(&record, value->Data, sizeof(record));
        loopz_driver::AdapterConfiguration configuration;
        if (!loopz_driver::decodeAdapterConfiguration(record, configuration))
            return STATUS_INVALID_PARAMETER;
        m_AudioFormat = configuration.format;
        m_ExposeCaptureEndpoint = configuration.exposesMicrophone() ? TRUE : FALSE;
    }
    const ULONG ingress = loopz_driver::FloatDuplexBridge::ingressBytes(m_AudioFormat);
    const ULONG recording = loopz_driver::FloatDuplexBridge::recordingBytes(m_AudioFormat);
    m_AudioStorage =
        static_cast<PBYTE>(ExAllocatePool2(POOL_FLAG_NON_PAGED, ingress + recording, 'fZLL'));
    if (!m_AudioStorage)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (!m_Bridge.configure(m_AudioFormat, m_AudioStorage, ingress, m_AudioStorage + ingress,
                            recording))
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS
CAdapterCommon::Init(_In_ PDEVICE_OBJECT DeviceObject)
/*++

Routine Description:

    Initialize adapter common object.

Arguments:

    DeviceObject - pointer to the device object

Return Value:

  NT status code.

--*/
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::Init]"));

    ASSERT(DeviceObject);
    KeInitializeSpinLock(&m_BridgeLock);
    m_Bridge.playback(false);

    NTSTATUS ntStatus = STATUS_SUCCESS;

    m_pServiceGroupWave = NULL;
    m_pDeviceObject = DeviceObject;
    m_pPhysicalDeviceObject = NULL;
    m_WdfDevice = NULL;
    m_PowerState = PowerDeviceD0;
    m_pHW = NULL;
    m_pPortClsEtwHelper = NULL;

    InitializeListHead(&m_SubdeviceCache);

    //
    // Get the PDO.
    //
    ntStatus = PcGetPhysicalDeviceObject(DeviceObject, &m_pPhysicalDeviceObject);
    IF_FAILED_ACTION_JUMP(ntStatus,
                          DPF(D_ERROR, ("PcGetPhysicalDeviceObject failed, 0x%x", ntStatus)), Done);

    ntStatus = LoadAudioFormat();
    IF_FAILED_JUMP(ntStatus, Done);

    //
    // Create a WDF miniport to represent the adapter. Note that WDF miniports
    // are NOT audio miniports. An audio adapter is associated with a single WDF
    // miniport. This driver uses WDF to simplify the handling of the Bluetooth
    // SCO HFP Bypass interface.
    //
    ntStatus = WdfDeviceMiniportCreate(WdfGetDriver(), WDF_NO_OBJECT_ATTRIBUTES,
                                       DeviceObject, // FDO
                                       NULL,         // Next device.
                                       NULL,         // PDO
                                       &m_WdfDevice);
    IF_FAILED_ACTION_JUMP(ntStatus,
                          DPF(D_ERROR, ("WdfDeviceMiniportCreate failed, 0x%x", ntStatus)), Done);

    // Initialize HW.
    //
    m_pHW = new (POOL_FLAG_NON_PAGED, SIMPLEAUDIOSAMPLE_POOLTAG) CSimpleAudioSampleHW;
    if (!m_pHW) {
        DPF(D_TERSE, ("Insufficient memory for Simple Audio Sample HW"));
        ntStatus = STATUS_INSUFFICIENT_RESOURCES;
    }
    IF_FAILED_JUMP(ntStatus, Done);

    m_pHW->MixerReset();

Done:

    return ntStatus;
} // Init

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(void)
CAdapterCommon::MixerReset(void)
/*++

Routine Description:

  Reset mixer registers from registry.

Arguments:

Return Value:

  void

--*/
{
    PAGED_CODE();

    if (m_pHW) {
        m_pHW->MixerReset();
    }
} // MixerReset

//=============================================================================
/* Here are the definitions of the standard miniport events.

Event type  : eMINIPORT_IHV_DEFINED 
Parameter 1 : Defined and used by IHVs
Parameter 2 : Defined and used by IHVs
Parameter 3 : Defined and used by IHVs
Parameter 4 :Defined and used by IHVs

Event type: eMINIPORT_BUFFER_COMPLETE
Parameter 1: Current linear buffer position    
Parameter 2: the previous WaveRtBufferWritePosition that the drive received    
Parameter 3: Data length completed
Parameter 4:0

Event type: eMINIPORT_PIN_STATE
Parameter 1: Current linear buffer position    
Parameter 2: the previous WaveRtBufferWritePosition that the drive received        
Parameter 3: Pin State 0->KS_STOP, 1->KS_ACQUIRE, 2->KS_PAUSE, 3->KS_RUN 
Parameter 4:0

Event type: eMINIPORT_GET_STREAM_POS
Parameter 1: Current linear buffer position    
Parameter 2: the previous WaveRtBufferWritePosition that the drive received    
Parameter 3: 0
Parameter 4:0


Event type: eMINIPORT_SET_WAVERT_BUFFER_WRITE_POS
Parameter 1: Current linear buffer position    
Parameter 2: the previous WaveRtBufferWritePosition that the drive received    
Parameter 3: the arget WaveRtBufferWritePosition received from portcls
Parameter 4:0

Event type: eMINIPORT_GET_PRESENTATION_POS
Parameter 1: Current linear buffer position    
Parameter 2: the previous WaveRtBufferWritePosition that the drive received    
Parameter 3: Presentation position
Parameter 4:0

Event type: eMINIPORT_PROGRAM_DMA 
Parameter 1: Current linear buffer position    
Parameter 2: the previous WaveRtBufferWritePosition that the drive received    
Parameter 3: Starting  WaveRt buffer offset
Parameter 4: Data length

Event type: eMINIPORT_GLITCH_REPORT
Parameter 1: Current linear buffer position    
Parameter 2: the previous WaveRtBufferWritePosition that the drive received    
Parameter 3: major glitch code: 1:WaveRT buffer is underrun, 
                                2:decoder errors, 
                                3:receive the same wavert buffer two in a row in event driven mode
Parameter 4: minor code for the glitch cause

Event type: eMINIPORT_LAST_BUFFER_RENDERED
Parameter 1: Current linear buffer position    
Parameter 2: the very last WaveRtBufferWritePosition that the driver received    
Parameter 3: 0
Parameter 4: 0

*/
#pragma code_seg()
STDMETHODIMP
CAdapterCommon::WriteEtwEvent(_In_ EPcMiniportEngineEvent miniportEventType,
                              _In_ ULONGLONG ullData1, _In_ ULONGLONG ullData2,
                              _In_ ULONGLONG ullData3, _In_ ULONGLONG ullData4) {
    NTSTATUS ntStatus = STATUS_SUCCESS;

    if (m_pPortClsEtwHelper) {
        ntStatus = m_pPortClsEtwHelper->MiniportWriteEtwEvent(miniportEventType, ullData1, ullData2,
                                                              ullData3, ullData4);
    }
    return ntStatus;
} // WriteEtwEvent

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(void)
CAdapterCommon::SetEtwHelper(PPORTCLSETWHELPER _pPortClsEtwHelper) {
    PAGED_CODE();

    SAFE_RELEASE(m_pPortClsEtwHelper);

    m_pPortClsEtwHelper = _pPortClsEtwHelper;

    if (m_pPortClsEtwHelper) {
        m_pPortClsEtwHelper->AddRef();
    }
} // SetEtwHelper

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP
CAdapterCommon::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID* Object)
/*++

Routine Description:

  QueryInterface routine for AdapterCommon

Arguments:

  Interface - 

  Object -

Return Value:

  NT status code.

--*/
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PADAPTERCOMMON(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IAdapterCommon)) {
        *Object = PVOID(PADAPTERCOMMON(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement)) {
        *Object = PVOID(PADAPTERPOWERMANAGEMENT(this));
    } else {
        *Object = NULL;
    }

    if (*Object) {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
} // NonDelegatingQueryInterface

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(void)
CAdapterCommon::SetWaveServiceGroup(_In_ PSERVICEGROUP ServiceGroup)
/*++

Routine Description:


Arguments:

Return Value:

  NT status code.

--*/
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::SetWaveServiceGroup]"));

    SAFE_RELEASE(m_pServiceGroupWave);

    m_pServiceGroupWave = ServiceGroup;

    if (m_pServiceGroupWave) {
        m_pServiceGroupWave->AddRef();
    }
} // SetWaveServiceGroup

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(BOOL)
CAdapterCommon::bDevSpecificRead()
/*++

Routine Description:

  Fetch Device Specific information.

Arguments:

  N/A

Return Value:

    BOOL - Device Specific info

--*/
{
    if (m_pHW) {
        return m_pHW->bGetDevSpecific();
    }

    return FALSE;
} // bDevSpecificRead

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::bDevSpecificWrite(_In_ BOOL bDevSpecific)
/*++

Routine Description:

  Store the new value in the Device Specific location.

Arguments:

  bDevSpecific - Value to store

Return Value:

  N/A.

--*/
{
    if (m_pHW) {
        m_pHW->bSetDevSpecific(bDevSpecific);
    }
} // DevSpecificWrite

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(INT)
CAdapterCommon::iDevSpecificRead()
/*++

Routine Description:

  Fetch Device Specific information.

Arguments:

  N/A

Return Value:

    INT - Device Specific info

--*/
{
    if (m_pHW) {
        return m_pHW->iGetDevSpecific();
    }

    return 0;
} // iDevSpecificRead

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::iDevSpecificWrite(_In_ INT iDevSpecific)
/*++

Routine Description:

  Store the new value in the Device Specific location.

Arguments:

  iDevSpecific - Value to store

Return Value:

  N/A.

--*/
{
    if (m_pHW) {
        m_pHW->iSetDevSpecific(iDevSpecific);
    }
} // iDevSpecificWrite

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(UINT)
CAdapterCommon::uiDevSpecificRead()
/*++

Routine Description:

  Fetch Device Specific information.

Arguments:

  N/A

Return Value:

    UINT - Device Specific info

--*/
{
    if (m_pHW) {
        return m_pHW->uiGetDevSpecific();
    }

    return 0;
} // uiDevSpecificRead

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::uiDevSpecificWrite(_In_ UINT uiDevSpecific)
/*++

Routine Description:

  Store the new value in the Device Specific location.

Arguments:

  uiDevSpecific - Value to store

Return Value:

  N/A.

--*/
{
    if (m_pHW) {
        m_pHW->uiSetDevSpecific(uiDevSpecific);
    }
} // uiDevSpecificWrite

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(BOOL)
CAdapterCommon::MixerMuteRead(_In_ ULONG Index, _In_ ULONG Channel)
/*++

Routine Description:

  Store the new value in mixer register array.

Arguments:

  Index - node id

Return Value:

    BOOL - mixer mute setting for this node

--*/
{
    if (m_pHW) {
        return m_pHW->GetMixerMute(Index, Channel);
    }

    return 0;
} // MixerMuteRead

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::MixerMuteWrite(_In_ ULONG Index, _In_ ULONG Channel, _In_ BOOL Value)
/*++

Routine Description:

  Store the new value in mixer register array.

Arguments:

  Index - node id

  Value - new mute settings

Return Value:

  NT status code.

--*/
{
    if (m_pHW) {
        m_pHW->SetMixerMute(Index, Channel, Value);
    }
} // MixerMuteWrite

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(ULONG)
CAdapterCommon::MixerMuxRead()
/*++

Routine Description:

  Return the mux selection

Arguments:

  Index - node id

  Value - new mute settings

Return Value:

  NT status code.

--*/
{
    if (m_pHW) {
        return m_pHW->GetMixerMux();
    }

    return 0;
} // MixerMuxRead

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::MixerMuxWrite(_In_ ULONG Index)
/*++

Routine Description:

  Store the new mux selection

Arguments:

  Index - node id

  Value - new mute settings

Return Value:

  NT status code.

--*/
{
    if (m_pHW) {
        m_pHW->SetMixerMux(Index);
    }
} // MixerMuxWrite

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(LONG)
CAdapterCommon::MixerVolumeRead(_In_ ULONG Index, _In_ ULONG Channel)
/*++

Routine Description:

  Return the value in mixer register array.

Arguments:

  Index - node id

  Channel = which channel

Return Value:

    Byte - mixer volume settings for this line

--*/
{
    if (m_pHW) {
        return m_pHW->GetMixerVolume(Index, Channel);
    }

    return 0;
} // MixerVolumeRead

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::MixerVolumeWrite(_In_ ULONG Index, _In_ ULONG Channel, _In_ LONG Value)
/*++

Routine Description:

  Store the new value in mixer register array.

Arguments:

  Index - node id

  Channel - which channel

  Value - new volume level

Return Value:

    void

--*/
{
    if (m_pHW) {
        m_pHW->SetMixerVolume(Index, Channel, Value);
    }
} // MixerVolumeWrite

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(LONG)
CAdapterCommon::MixerPeakMeterRead(_In_ ULONG Index, _In_ ULONG Channel)
/*++

Routine Description:

  Return the value in mixer register array.

Arguments:

  Index - node id

  Channel = which channel

Return Value:

    Byte - mixer sample peak meter settings for this line

--*/
{
    if (m_pHW) {
        return m_pHW->GetMixerPeakMeter(Index, Channel);
    }

    return 0;
} // MixerVolumeRead

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::PowerChangeState(_In_ POWER_STATE NewState)
/*++

Routine Description:


Arguments:

  NewState - The requested, new power state for the device. 

Return Value:

    void

Note:
  From MSDN:

  To assist the driver, PortCls will pause any active audio streams prior to calling
  this method to place the device in a sleep state. After calling this method, PortCls
  will unpause active audio streams, to wake the device up. Miniports can opt for 
  additional notification by utilizing the IPowerNotify interface.

  The miniport driver must perform the requested change to the device's power state 
  before it returns from the PowerChangeState call. If the miniport driver needs to 
  save or restore any device state before a power-state change, the miniport driver 
  should support the IPowerNotify interface, which allows it to receive advance warning
  of any such change. Before returning from a successful PowerChangeState call, the 
  miniport driver should cache the new power state.

  While the miniport driver is in one of the sleep states (any state other than 
  PowerDeviceD0), it must avoid writing to the hardware. The miniport driver must cache
  any hardware accesses that need to be deferred until the device powers up again. If
  the power state is changing from one of the sleep states to PowerDeviceD0, the 
  miniport driver should perform any deferred hardware accesses after it has powered up
  the device. If the power state is changing from PowerDeviceD0 to a sleep state, the 
  miniport driver can perform any necessary hardware accesses during the PowerChangeState
  call before it powers down the device.

  While powered down, a miniport driver is never asked to create a miniport driver object
  or stream object. PortCls always places the device in the PowerDeviceD0 state before
  calling the miniport driver's NewStream method.
  
--*/
{
    DPF_ENTER(("[CAdapterCommon::PowerChangeState]"));

    // Notify all registered miniports of a power state change
    PLIST_ENTRY le = NULL;
    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache; le = le->Flink) {
        MINIPAIR_UNKNOWN* pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);

        if (pRecord->PowerInterface) {
            pRecord->PowerInterface->PowerChangeState(NewState);
        }
    }

    // is this actually a state change??
    //
    if (NewState.DeviceState != m_PowerState) {
        // switch on new state
        //
        switch (NewState.DeviceState) {
        case PowerDeviceD0:
        case PowerDeviceD1:
        case PowerDeviceD2:
        case PowerDeviceD3:
            m_PowerState = NewState.DeviceState;

            DPF(D_VERBOSE, ("Entering D%u", ULONG(m_PowerState) - ULONG(PowerDeviceD0)));

            break;

        default:

            DPF(D_VERBOSE, ("Unknown Device Power State"));
            break;
        }
    }
} // PowerStateChange

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::QueryDeviceCapabilities(_Inout_updates_bytes_(sizeof(DEVICE_CAPABILITIES))
                                            PDEVICE_CAPABILITIES PowerDeviceCaps)
/*++

Routine Description:

    Called at startup to get the caps for the device.  This structure provides 
    the system with the mappings between system power state and device power 
    state.  This typically will not need modification by the driver.         

Arguments:

  PowerDeviceCaps - The device's capabilities. 

Return Value:

  NT status code.

--*/
{
    UNREFERENCED_PARAMETER(PowerDeviceCaps);

    DPF_ENTER(("[CAdapterCommon::QueryDeviceCapabilities]"));

    return (STATUS_SUCCESS);
} // QueryDeviceCapabilities

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::QueryPowerChangeState(_In_ POWER_STATE NewStateQuery)
/*++

Routine Description:

  Query to see if the device can change to this power state 

Arguments:

  NewStateQuery - The requested, new power state for the device

Return Value:

  NT status code.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    DPF_ENTER(("[CAdapterCommon::QueryPowerChangeState]"));

    // query each miniport for it's power state, we're finished if even one indicates
    // it cannot go to this power state.
    PLIST_ENTRY le = NULL;
    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache && NT_SUCCESS(status);
         le = le->Flink) {
        MINIPAIR_UNKNOWN* pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);

        if (pRecord->PowerInterface) {
            status = pRecord->PowerInterface->QueryPowerChangeState(NewStateQuery);
        }
    }

    return status;
} // QueryPowerChangeState

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::CreateAudioInterfaceWithProperties(
    _In_ PCWSTR ReferenceString, _In_opt_ PCWSTR TemplateReferenceString, _In_ ULONG cPropertyCount,
    _In_reads_opt_(cPropertyCount) const SIMPLEAUDIOSAMPLE_DEVPROPERTY* pProperties,
    _Out_ _At_(AudioSymbolicLinkName->Buffer, __drv_allocatesMem(Mem))
        PUNICODE_STRING AudioSymbolicLinkName,
    _In_opt_ const GUID* InterfaceCategory)
/*++

Routine Description:

Create the audio interface (in disabled mode).

--*/
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::CreateAudioInterfaceWithProperties]"));

    NTSTATUS ntStatus;
    UNICODE_STRING referenceString;
    UNICODE_STRING speaker = RTL_CONSTANT_STRING(L"TopologySpeaker");
    UNICODE_STRING microphone = RTL_CONSTANT_STRING(L"TopologyMicArray1");
    UNICODE_STRING templateReference;
    RtlInitUnicodeString(&templateReference,
                         TemplateReferenceString ? TemplateReferenceString : ReferenceString);

    RtlInitUnicodeString(&referenceString, ReferenceString);

    //
    // Reset output value.
    //
    RtlZeroMemory(AudioSymbolicLinkName, sizeof(UNICODE_STRING));

    //
    // Register an audio interface if not already present.
    //
    ntStatus = IoRegisterDeviceInterface(GetPhysicalDeviceObject(),
                                         InterfaceCategory ? InterfaceCategory : &KSCATEGORY_AUDIO,
                                         &referenceString, AudioSymbolicLinkName);

    IF_FAILED_ACTION_JUMP(ntStatus,
                          DPF(D_ERROR, ("CreateAudioInterfaceWithProperties: "
                                        "IoRegisterDeviceInterface(KSCATEGORY_AUDIO): failed, 0x%x",
                                        ntStatus)),
                          Done);

    //
    // Migrate optional device interface parameters from the template if it exists
    // This is done first, so that any additional parameters in pProperties will override the defaults.
    //
    if (NULL != TemplateReferenceString) {
        ntStatus = MigrateDeviceInterfaceTemplateParameters(AudioSymbolicLinkName,
                                                            TemplateReferenceString);

        IF_FAILED_ACTION_JUMP(
            ntStatus,
            DPF(D_ERROR, ("MigrateDeviceInterfaceTempalteParameters: "
                          "MigrateDeviceInterfaceTemplateParameters(...): failed, 0x%x",
                          ntStatus)),
            Done);
    }

    //
    // Set properties on the interface
    //
    ntStatus = SimpleAudioSampleIoSetDeviceInterfacePropertyDataMultiple(
        AudioSymbolicLinkName, cPropertyCount, pProperties);

    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR,
            ("CreateAudioInterfaceWithProperties: "
             "SimpleAudioSampleIoSetDeviceInterfacePropertyDataMultiple(...): failed, 0x%x",
             ntStatus)),
        Done);

    if (RtlEqualUnicodeString(&templateReference, &speaker, TRUE) ||
        RtlEqualUnicodeString(&templateReference, &microphone, TRUE)) {
        ntStatus = PublishAudioFormat(AudioSymbolicLinkName);
        IF_FAILED_JUMP(ntStatus, Done);
    }

    //
    // All done.
    //
    ntStatus = STATUS_SUCCESS;

Done:
    if (!NT_SUCCESS(ntStatus)) {
        RtlFreeUnicodeString(AudioSymbolicLinkName);
        RtlZeroMemory(AudioSymbolicLinkName, sizeof(UNICODE_STRING));
    }
    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::InstallSubdevice(_In_opt_ PIRP Irp, _In_ PWSTR Name, _In_opt_ PWSTR TemplateName,
                                 _In_ REFGUID PortClassId, _In_ REFGUID MiniportClassId,
                                 _In_opt_ PFNCREATEMINIPORT MiniportCreate,
                                 _In_ ULONG cPropertyCount,
                                 _In_reads_opt_(cPropertyCount)
                                     const SIMPLEAUDIOSAMPLE_DEVPROPERTY* pProperties,
                                 _In_opt_ PVOID DeviceContext, _In_ PENDPOINT_MINIPAIR MiniportPair,
                                 _In_opt_ PRESOURCELIST ResourceList, _In_ REFGUID PortInterfaceId,
                                 _Out_opt_ PUNKNOWN* OutPortInterface,
                                 _Out_opt_ PUNKNOWN* OutPortUnknown,
                                 _Out_opt_ PUNKNOWN* OutMiniportUnknown) {
    /*++

Routine Description:

    This function creates and registers a subdevice consisting of a port       
    driver, a minport driver and a set of resources bound together.  It will   
    also optionally place a pointer to an interface on the port driver in a    
    specified location before initializing the port driver.  This is done so   
    that a common ISR can have access to the port driver during 
    initialization, when the ISR might fire.                                   

Arguments:

    Irp - pointer to the irp object.

    Name - name of the miniport. Passes to PcRegisterSubDevice
 
    PortClassId - port class id. Passed to PcNewPort.

    MiniportClassId - miniport class id. Passed to PcNewMiniport.

    MiniportCreate - pointer to a miniport creation function. If NULL, 
                     PcNewMiniport is used.
                     
    DeviceContext - deviceType specific.

    MiniportPair - endpoint configuration info.    

    ResourceList - pointer to the resource list.

    PortInterfaceId - GUID that represents the port interface.
       
    OutPortInterface - pointer to store the port interface

    OutPortUnknown - pointer to store the unknown port interface.

    OutMiniportUnknown - pointer to store the unknown miniport interface

Return Value:

    NT status code.

--*/
    PAGED_CODE();
    DPF_ENTER(("[InstallSubDevice %S]", Name));

    ASSERT(Name != NULL);
    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS ntStatus;
    PPORT port = NULL;
    PUNKNOWN miniport = NULL;
    PADAPTERCOMMON adapterCommon = NULL;
    UNICODE_STRING symbolicLink = {0};

    adapterCommon = PADAPTERCOMMON(this);

    // Windows 10 endpoint migration cannot disambiguate multiple software
    // adapters sharing the same hardware ID and static reference strings.
    // Derive registration names from the durable PnP root instance; use the
    // existing INF names only as property templates and per-adapter cache keys.
    WCHAR instance[200]{};
    if (m_EndpointReferenceCount >= ARRAYSIZE(m_EndpointReferences))
        return STATUS_INSUFFICIENT_RESOURCES;
    auto* uniqueName = m_EndpointReferences[m_EndpointReferenceCount++];
    ULONG instanceBytes{};
    DEVPROPTYPE instanceType{};
    ntStatus =
        IoGetDevicePropertyData(m_pPhysicalDeviceObject, &DEVPKEY_Device_InstanceId, LOCALE_NEUTRAL,
                                0, sizeof(instance), instance, &instanceBytes, &instanceType);
    if (!NT_SUCCESS(ntStatus))
        return ntStatus;
    if (instanceType != DEVPROP_TYPE_STRING || instanceBytes < sizeof(WCHAR) ||
        instanceBytes > sizeof(instance) || instanceBytes % sizeof(WCHAR) ||
        instance[instanceBytes / sizeof(WCHAR) - 1] != 0 ||
        !loopz_driver::endpointReference(instance, Name, uniqueName,
                                         ARRAYSIZE(m_EndpointReferences[0])))
        return STATUS_INVALID_PARAMETER;

    // Preinitialize the same categories declared by the static INF templates.
    // PortCls enables these interfaces when it registers the bound subdevice.
    const GUID* categories[] = {&KSCATEGORY_AUDIO, &KSCATEGORY_REALTIME,
                                MiniportPair->DeviceType == eSpeakerDevice ? &KSCATEGORY_RENDER
                                                                           : &KSCATEGORY_CAPTURE};
    ULONG categoryCount = ARRAYSIZE(categories);
    if (IsEqualGUIDAligned(PortClassId, CLSID_PortTopology)) {
        categories[1] = &KSCATEGORY_TOPOLOGY;
        categoryCount = 2;
    }
    for (ULONG category = 0; category < categoryCount; ++category) {
        ntStatus = CreateAudioInterfaceWithProperties(
            uniqueName, TemplateName ? TemplateName : Name, cPropertyCount, pProperties,
            &symbolicLink, categories[category]);
        if (!NT_SUCCESS(ntStatus))
            return ntStatus;
        RtlFreeUnicodeString(&symbolicLink);
        RtlZeroMemory(&symbolicLink, sizeof(symbolicLink));
    }
    ntStatus = PcNewPort(&port, PortClassId);

    // Create the miniport object
    //
    if (NT_SUCCESS(ntStatus)) {
        if (MiniportCreate) {
            ntStatus = MiniportCreate(&miniport, MiniportClassId, NULL, POOL_FLAG_NON_PAGED,
                                      adapterCommon, DeviceContext, MiniportPair);
        } else {
            ntStatus = PcNewMiniport((PMINIPORT*)&miniport, MiniportClassId);
        }
    }

    // Init the port driver and miniport in one go.
    //
    if (NT_SUCCESS(ntStatus)) {
#pragma warning(push)
        // IPort::Init's annotation on ResourceList requires it to be non-NULL.  However,
        // for dynamic devices, we may no longer have the resource list and this should
        // still succeed.
        //
#pragma warning(disable : 6387)
        ntStatus = port->Init(m_pDeviceObject, Irp, miniport, adapterCommon, ResourceList);
#pragma warning(pop)

        if (NT_SUCCESS(ntStatus)) {
            // Register the subdevice (port/miniport combination).
            //
            ntStatus = PcRegisterSubdevice(m_pDeviceObject, uniqueName, port);
        }
    }

    // Deposit the port interfaces if it's needed.
    //
    if (NT_SUCCESS(ntStatus)) {
        if (OutPortUnknown) {
            ntStatus = port->QueryInterface(IID_IUnknown, (PVOID*)OutPortUnknown);
        }

        if (OutPortInterface) {
            ntStatus = port->QueryInterface(PortInterfaceId, (PVOID*)OutPortInterface);
        }

        if (OutMiniportUnknown) {
            ntStatus = miniport->QueryInterface(IID_IUnknown, (PVOID*)OutMiniportUnknown);
        }
    }

    if (port) {
        port->Release();
    }

    if (miniport) {
        miniport->Release();
    }

    return ntStatus;
} // InstallSubDevice

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::UnregisterSubdevice(_In_opt_ PUNKNOWN UnknownPort)
/*++

Routine Description:

  Unregisters and releases the specified subdevice.

Arguments:

  UnknownPort - Wave or topology port interface.

Return Value:

  NTSTATUS

--*/
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::UnregisterSubdevice]"));

    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS ntStatus = STATUS_SUCCESS;
    PUNREGISTERSUBDEVICE unregisterSubdevice = NULL;

    if (NULL == UnknownPort) {
        return ntStatus;
    }

    //
    // Get the IUnregisterSubdevice interface.
    //
    ntStatus = UnknownPort->QueryInterface(IID_IUnregisterSubdevice, (PVOID*)&unregisterSubdevice);

    //
    // Unregister the port object.
    //
    if (NT_SUCCESS(ntStatus)) {
        ntStatus = unregisterSubdevice->UnregisterSubdevice(m_pDeviceObject, UnknownPort);

        //
        // Release the IUnregisterSubdevice interface.
        //
        unregisterSubdevice->Release();
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::ConnectTopologies(_In_ PUNKNOWN UnknownTopology, _In_ PUNKNOWN UnknownWave,
                                  _In_ PHYSICALCONNECTIONTABLE* PhysicalConnections,
                                  _In_ ULONG PhysicalConnectionCount)
/*++

Routine Description:

  Connects the bridge pins between the wave and mixer topologies.

Arguments:

Return Value:

  NTSTATUS

--*/
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::ConnectTopologies]"));

    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS ntStatus = STATUS_SUCCESS;

    //
    // register wave <=> topology connections
    // This will connect bridge pins of wave and topology
    // miniports.
    //
    for (ULONG i = 0; i < PhysicalConnectionCount && NT_SUCCESS(ntStatus); i++) {

        switch (PhysicalConnections[i].eType) {
        case CONNECTIONTYPE_TOPOLOGY_OUTPUT:
            ntStatus = PcRegisterPhysicalConnection(m_pDeviceObject, UnknownTopology,
                                                    PhysicalConnections[i].ulTopology, UnknownWave,
                                                    PhysicalConnections[i].ulWave);
            if (!NT_SUCCESS(ntStatus)) {
                DPF(D_TERSE,
                    ("ConnectTopologies: PcRegisterPhysicalConnection(render) failed, 0x%x",
                     ntStatus));
            }
            break;
        case CONNECTIONTYPE_WAVE_OUTPUT:
            ntStatus = PcRegisterPhysicalConnection(m_pDeviceObject, UnknownWave,
                                                    PhysicalConnections[i].ulWave, UnknownTopology,
                                                    PhysicalConnections[i].ulTopology);
            if (!NT_SUCCESS(ntStatus)) {
                DPF(D_TERSE,
                    ("ConnectTopologies: PcRegisterPhysicalConnection(capture) failed, 0x%x",
                     ntStatus));
            }
            break;
        }
    }

    //
    // Cleanup in case of error.
    //
    if (!NT_SUCCESS(ntStatus)) {
        // disconnect all connections on error, ignore error code because not all
        // connections may have been made
        DisconnectTopologies(UnknownTopology, UnknownWave, PhysicalConnections,
                             PhysicalConnectionCount);
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::DisconnectTopologies(_In_ PUNKNOWN UnknownTopology, _In_ PUNKNOWN UnknownWave,
                                     _In_ PHYSICALCONNECTIONTABLE* PhysicalConnections,
                                     _In_ ULONG PhysicalConnectionCount)
/*++

Routine Description:

  Disconnects the bridge pins between the wave and mixer topologies.

Arguments:

Return Value:

  NTSTATUS

--*/
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::DisconnectTopologies]"));

    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS ntStatus = STATUS_SUCCESS;
    NTSTATUS ntStatus2 = STATUS_SUCCESS;
    PUNREGISTERPHYSICALCONNECTION unregisterPhysicalConnection = NULL;

    //
    // Get the IUnregisterPhysicalConnection interface
    //
    ntStatus = UnknownTopology->QueryInterface(IID_IUnregisterPhysicalConnection,
                                               (PVOID*)&unregisterPhysicalConnection);

    if (NT_SUCCESS(ntStatus)) {
        for (ULONG i = 0; i < PhysicalConnectionCount; i++) {
            switch (PhysicalConnections[i].eType) {
            case CONNECTIONTYPE_TOPOLOGY_OUTPUT:
                ntStatus = unregisterPhysicalConnection->UnregisterPhysicalConnection(
                    m_pDeviceObject, UnknownTopology, PhysicalConnections[i].ulTopology,
                    UnknownWave, PhysicalConnections[i].ulWave);

                if (!NT_SUCCESS(ntStatus)) {
                    DPF(D_TERSE,
                        ("DisconnectTopologies: UnregisterPhysicalConnection(render) failed, 0x%x",
                         ntStatus));
                }
                break;
            case CONNECTIONTYPE_WAVE_OUTPUT:
                ntStatus = unregisterPhysicalConnection->UnregisterPhysicalConnection(
                    m_pDeviceObject, UnknownWave, PhysicalConnections[i].ulWave, UnknownTopology,
                    PhysicalConnections[i].ulTopology);
                if (!NT_SUCCESS(ntStatus2)) {
                    DPF(D_TERSE,
                        ("DisconnectTopologies: UnregisterPhysicalConnection(capture) failed, 0x%x",
                         ntStatus2));
                }
                break;
            }

            // cache and return the first error encountered, as it's likely the most relevent
            if (NT_SUCCESS(ntStatus)) {
                ntStatus = ntStatus2;
            }
        }
    }

    //
    // Release the IUnregisterPhysicalConnection interface.
    //
    SAFE_RELEASE(unregisterPhysicalConnection);

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::GetCachedSubdevice(_In_ PWSTR Name, _Out_opt_ PUNKNOWN* OutUnknownPort,
                                   _Out_opt_ PUNKNOWN* OutUnknownMiniport) {
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::GetCachedSubdevice]"));

    // search list, return interface to device if found, fail if not found
    PLIST_ENTRY le = NULL;
    BOOL bFound = FALSE;

    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache && !bFound; le = le->Flink) {
        MINIPAIR_UNKNOWN* pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);

        if (0 == wcscmp(Name, pRecord->Name)) {
            if (OutUnknownPort) {
                *OutUnknownPort = pRecord->PortInterface;
                (*OutUnknownPort)->AddRef();
            }

            if (OutUnknownMiniport) {
                *OutUnknownMiniport = pRecord->MiniportInterface;
                (*OutUnknownMiniport)->AddRef();
            }

            bFound = TRUE;
        }
    }

    return bFound ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::CacheSubdevice(_In_ PWSTR Name, _In_ PUNKNOWN UnknownPort,
                               _In_ PUNKNOWN UnknownMiniport) {
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::CacheSubdevice]"));

    // add the item with this name/interface to the list
    NTSTATUS ntStatus = STATUS_SUCCESS;
    MINIPAIR_UNKNOWN* pNewSubdevice = NULL;

    pNewSubdevice = new (POOL_FLAG_NON_PAGED, MINADAPTER_POOLTAG) MINIPAIR_UNKNOWN;

    if (!pNewSubdevice) {
        DPF(D_TERSE, ("Insufficient memory to cache subdevice"));
        ntStatus = STATUS_INSUFFICIENT_RESOURCES;
    }

    if (NT_SUCCESS(ntStatus)) {
        memset(pNewSubdevice, 0, sizeof(MINIPAIR_UNKNOWN));

        ntStatus = RtlStringCchCopyW(pNewSubdevice->Name, SIZEOF_ARRAY(pNewSubdevice->Name), Name);
    }

    if (NT_SUCCESS(ntStatus)) {
        pNewSubdevice->PortInterface = UnknownPort;
        pNewSubdevice->PortInterface->AddRef();

        pNewSubdevice->MiniportInterface = UnknownMiniport;
        pNewSubdevice->MiniportInterface->AddRef();

        // cache the IAdapterPowerManagement interface (if available) from the filter. Some endpoints,
        // like FM and cellular, have their own power requirements that we must track. If this fails,
        // it just means this filter doesn't do power management.
        UnknownMiniport->QueryInterface(IID_IAdapterPowerManagement,
                                        (PVOID*)&(pNewSubdevice->PowerInterface));

        InsertTailList(&m_SubdeviceCache, &pNewSubdevice->ListEntry);
    }

    if (!NT_SUCCESS(ntStatus)) {
        if (pNewSubdevice) {
            delete pNewSubdevice;
        }
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::RemoveCachedSubdevice(_In_ PWSTR Name) {
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::RemoveCachedSubdevice]"));

    // search list, remove the entry from the list

    PLIST_ENTRY le = NULL;
    BOOL bRemoved = FALSE;

    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache && !bRemoved; le = le->Flink) {
        MINIPAIR_UNKNOWN* pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);

        if (0 == wcscmp(Name, pRecord->Name)) {
            SAFE_RELEASE(pRecord->PortInterface);
            SAFE_RELEASE(pRecord->MiniportInterface);
            SAFE_RELEASE(pRecord->PowerInterface);
            memset(pRecord->Name, 0, sizeof(pRecord->Name));
            RemoveEntryList(le);
            bRemoved = TRUE;
            delete pRecord;
            break;
        }
    }

    return bRemoved ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

#pragma code_seg("PAGE")
VOID CAdapterCommon::EmptySubdeviceCache() {
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::EmptySubdeviceCache]"));

    while (!IsListEmpty(&m_SubdeviceCache)) {
        PLIST_ENTRY le = RemoveHeadList(&m_SubdeviceCache);
        MINIPAIR_UNKNOWN* pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);

        SAFE_RELEASE(pRecord->PortInterface);
        SAFE_RELEASE(pRecord->MiniportInterface);
        SAFE_RELEASE(pRecord->PowerInterface);
        memset(pRecord->Name, 0, sizeof(pRecord->Name));

        delete pRecord;
    }
}

#pragma code_seg("PAGE")
VOID CAdapterCommon::Cleanup() {
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::Cleanup]"));
    EmptySubdeviceCache();
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::InstallEndpointFilters(_In_opt_ PIRP Irp, _In_ PENDPOINT_MINIPAIR MiniportPair,
                                       _In_opt_ PVOID DeviceContext,
                                       _Out_opt_ PUNKNOWN* UnknownTopology,
                                       _Out_opt_ PUNKNOWN* UnknownWave,
                                       _Out_opt_ PUNKNOWN* UnknownMiniportTopology,
                                       _Out_opt_ PUNKNOWN* UnknownMiniportWave) {
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::InstallEndpointFilters]"));

    NTSTATUS ntStatus = STATUS_SUCCESS;
    PUNKNOWN unknownTopology = NULL;
    PUNKNOWN unknownWave = NULL;
    BOOL bTopologyCreated = FALSE;
    BOOL bWaveCreated = FALSE;
    PUNKNOWN unknownMiniTopo = NULL;
    PUNKNOWN unknownMiniWave = NULL;

    // Initialize output optional parameters if needed
    if (UnknownTopology) {
        *UnknownTopology = NULL;
    }

    if (UnknownWave) {
        *UnknownWave = NULL;
    }

    if (UnknownMiniportTopology) {
        *UnknownMiniportTopology = NULL;
    }

    if (UnknownMiniportWave) {
        *UnknownMiniportWave = NULL;
    }

    ntStatus = GetCachedSubdevice(MiniportPair->TopoName, &unknownTopology, &unknownMiniTopo);
    if (!NT_SUCCESS(ntStatus) || NULL == unknownTopology || NULL == unknownMiniTopo) {
        bTopologyCreated = TRUE;

        // Install Simple Audio Sample topology miniport for the render endpoint.
        //
        ntStatus = InstallSubdevice(
            Irp,
            MiniportPair
                ->TopoName, // make sure this name matches with SIMPLEAUDIOSAMPLE.<TopoName>.szPname in the inf's [Strings] section
            MiniportPair->TemplateTopoName, CLSID_PortTopology, CLSID_PortTopology,
            MiniportPair->TopoCreateCallback, MiniportPair->TopoInterfacePropertyCount,
            MiniportPair->TopoInterfaceProperties, DeviceContext, MiniportPair, NULL,
            IID_IPortTopology, NULL, &unknownTopology, &unknownMiniTopo);
        if (NT_SUCCESS(ntStatus)) {
            ntStatus = CacheSubdevice(MiniportPair->TopoName, unknownTopology, unknownMiniTopo);
        }
    }

    IF_FAILED_JUMP(ntStatus, Exit);

    ntStatus = GetCachedSubdevice(MiniportPair->WaveName, &unknownWave, &unknownMiniWave);
    if (!NT_SUCCESS(ntStatus) || NULL == unknownWave || NULL == unknownMiniWave) {
        bWaveCreated = TRUE;

        // Install Simple Audio Sample wave miniport for the render endpoint.
        //
        ntStatus = InstallSubdevice(
            Irp,
            MiniportPair
                ->WaveName, // make sure this name matches with SIMPLEAUDIOSAMPLE.<WaveName>.szPname in the inf's [Strings] section
            MiniportPair->TemplateWaveName, CLSID_PortWaveRT, CLSID_PortWaveRT,
            MiniportPair->WaveCreateCallback, MiniportPair->WaveInterfacePropertyCount,
            MiniportPair->WaveInterfaceProperties, DeviceContext, MiniportPair, NULL,
            IID_IPortWaveRT, NULL, &unknownWave, &unknownMiniWave);

        if (NT_SUCCESS(ntStatus)) {
            ntStatus = CacheSubdevice(MiniportPair->WaveName, unknownWave, unknownMiniWave);
        }
    }

    IF_FAILED_JUMP(ntStatus, Exit);

    if (unknownTopology && unknownWave) {
        //
        // register wave <=> topology connections
        // This will connect bridge pins of wave and topology
        // miniports.
        //
        ntStatus =
            ConnectTopologies(unknownTopology, unknownWave, MiniportPair->PhysicalConnections,
                              MiniportPair->PhysicalConnectionCount);
    }

Exit:
    if (NT_SUCCESS(ntStatus)) {
        //
        // Set output parameters.
        //
        if (UnknownTopology != NULL && unknownTopology != NULL) {
            unknownTopology->AddRef();
            *UnknownTopology = unknownTopology;
        }

        if (UnknownWave != NULL && unknownWave != NULL) {
            unknownWave->AddRef();
            *UnknownWave = unknownWave;
        }
        if (UnknownMiniportTopology != NULL && unknownMiniTopo != NULL) {
            unknownMiniTopo->AddRef();
            *UnknownMiniportTopology = unknownMiniTopo;
        }

        if (UnknownMiniportWave != NULL && unknownMiniWave != NULL) {
            unknownMiniWave->AddRef();
            *UnknownMiniportWave = unknownMiniWave;
        }

    } else {
        if (bTopologyCreated && unknownTopology != NULL) {
            UnregisterSubdevice(unknownTopology);
            RemoveCachedSubdevice(MiniportPair->TopoName);
        }

        if (bWaveCreated && unknownWave != NULL) {
            UnregisterSubdevice(unknownWave);
            RemoveCachedSubdevice(MiniportPair->WaveName);
        }
    }

    SAFE_RELEASE(unknownMiniTopo);
    SAFE_RELEASE(unknownTopology);
    SAFE_RELEASE(unknownMiniWave);
    SAFE_RELEASE(unknownWave);

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::RemoveEndpointFilters(_In_ PENDPOINT_MINIPAIR MiniportPair,
                                      _In_opt_ PUNKNOWN UnknownTopology,
                                      _In_opt_ PUNKNOWN UnknownWave) {
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::RemoveEndpointFilters]"));

    NTSTATUS ntStatus = STATUS_SUCCESS;

    if (UnknownTopology != NULL && UnknownWave != NULL) {
        ntStatus =
            DisconnectTopologies(UnknownTopology, UnknownWave, MiniportPair->PhysicalConnections,
                                 MiniportPair->PhysicalConnectionCount);

        if (!NT_SUCCESS(ntStatus)) {
            DPF(D_VERBOSE, ("RemoveEndpointFilters: DisconnectTopologies failed: 0x%x", ntStatus));
        }
    }

    RemoveCachedSubdevice(MiniportPair->WaveName);

    ntStatus = UnregisterSubdevice(UnknownWave);
    if (!NT_SUCCESS(ntStatus)) {
        DPF(D_VERBOSE, ("RemoveEndpointFilters: UnregisterSubdevice(wave) failed: 0x%x", ntStatus));
    }

    RemoveCachedSubdevice(MiniportPair->TopoName);

    ntStatus = UnregisterSubdevice(UnknownTopology);
    if (!NT_SUCCESS(ntStatus)) {
        DPF(D_VERBOSE,
            ("RemoveEndpointFilters: UnregisterSubdevice(topology) failed: 0x%x", ntStatus));
    }

    //
    // All Done.
    //
    ntStatus = STATUS_SUCCESS;

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::GetFilters(_In_ PENDPOINT_MINIPAIR MiniportPair,
                           _Out_opt_ PUNKNOWN* UnknownTopologyPort,
                           _Out_opt_ PUNKNOWN* UnknownTopologyMiniport,
                           _Out_opt_ PUNKNOWN* UnknownWavePort,
                           _Out_opt_ PUNKNOWN* UnknownWaveMiniport) {
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::GetFilters]"));

    NTSTATUS ntStatus = STATUS_SUCCESS;
    PUNKNOWN unknownTopologyPort = NULL;
    PUNKNOWN unknownTopologyMiniport = NULL;
    PUNKNOWN unknownWavePort = NULL;
    PUNKNOWN unknownWaveMiniport = NULL;

    // if the client requested the topology filter, find it and return it
    if (UnknownTopologyPort != NULL || UnknownTopologyMiniport != NULL) {
        ntStatus = GetCachedSubdevice(MiniportPair->TopoName, &unknownTopologyPort,
                                      &unknownTopologyMiniport);
        if (NT_SUCCESS(ntStatus)) {
            if (UnknownTopologyPort) {
                *UnknownTopologyPort = unknownTopologyPort;
            }

            if (UnknownTopologyMiniport) {
                *UnknownTopologyMiniport = unknownTopologyMiniport;
            }
        }
    }

    // if the client requested the wave filter, find it and return it
    if (NT_SUCCESS(ntStatus) && (UnknownWavePort != NULL || UnknownWaveMiniport != NULL)) {
        ntStatus =
            GetCachedSubdevice(MiniportPair->WaveName, &unknownWavePort, &unknownWaveMiniport);
        if (NT_SUCCESS(ntStatus)) {
            if (UnknownWavePort) {
                *UnknownWavePort = unknownWavePort;
            }

            if (UnknownWaveMiniport) {
                *UnknownWaveMiniport = unknownWaveMiniport;
            }
        }
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::SetIdlePowerManagement(_In_ PENDPOINT_MINIPAIR MiniportPair, _In_ BOOL bEnabled) {
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::SetIdlePowerManagement]"));

    NTSTATUS ntStatus = STATUS_SUCCESS;
    IUnknown* pUnknown = NULL;
    PPORTCLSPOWER pPortClsPower = NULL;
    // refcounting disable requests. Each miniport is responsible for calling this in pairs,
    // disable on the first request to disable, enable on the last request to enable.

    // make sure that we always call SetIdlePowerManagment using the IPortClsPower
    // from the requesting port, so we don't cache a reference to a port
    // indefinitely, preventing it from ever unloading.
    ntStatus = GetFilters(MiniportPair, NULL, NULL, &pUnknown, NULL);
    if (NT_SUCCESS(ntStatus)) {
        ntStatus = pUnknown->QueryInterface(IID_IPortClsPower, (PVOID*)&pPortClsPower);
    }

    if (NT_SUCCESS(ntStatus)) {
        if (bEnabled) {
            m_dwIdleRequests--;

            if (0 == m_dwIdleRequests) {
                pPortClsPower->SetIdlePowerManagement(m_pDeviceObject, TRUE);
            }
        } else {
            if (0 == m_dwIdleRequests) {
                pPortClsPower->SetIdlePowerManagement(m_pDeviceObject, FALSE);
            }

            m_dwIdleRequests++;
        }
    }

    SAFE_RELEASE(pUnknown);
    SAFE_RELEASE(pPortClsPower);

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS
CopyRegistryValues(HANDLE _hSourceKey, HANDLE _hDestinationKey)
/*++

Routine Description:

  This method copies the registry values in _hSourceKey to _hDestinationKey.

Return Value:

  NT status code.

--*/
{
    NTSTATUS ntStatus = STATUS_SUCCESS;
    PKEY_VALUE_FULL_INFORMATION kvFullInfo = NULL;
    ULONG ulFullInfoLength = 0;
    ULONG ulFullInfoResultLength = 0;
    PWSTR pwstrKeyValueName = NULL;
    UNICODE_STRING strKeyValueName;
    PAGED_CODE();
    // Allocate the KEY_VALUE_FULL_INFORMATION structure
    ulFullInfoLength = sizeof(KEY_VALUE_FULL_INFORMATION) + MAX_DEVICE_REG_KEY_LENGTH;
    kvFullInfo = (PKEY_VALUE_FULL_INFORMATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, ulFullInfoLength,
                                                              MINADAPTER_POOLTAG);
    IF_TRUE_ACTION_JUMP(kvFullInfo == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES, Exit);

    // Iterate over each value and copy it to the destination
    for (UINT i = 0; NT_SUCCESS(ntStatus); i++) {
        // Enumerate the next value
        ntStatus = ZwEnumerateValueKey(_hSourceKey, i, KeyValueFullInformation, kvFullInfo,
                                       ulFullInfoLength, &ulFullInfoResultLength);

        // Jump out of this loop if there are no more values
        IF_TRUE_ACTION_JUMP(ntStatus == STATUS_NO_MORE_ENTRIES, ntStatus = STATUS_SUCCESS, Exit);

        // Handle incorrect buffer size
        if (ntStatus == STATUS_BUFFER_TOO_SMALL || ntStatus == STATUS_BUFFER_OVERFLOW) {
            // Free and re-allocate the KEY_VALUE_FULL_INFORMATION structure with the correct size
            ExFreePoolWithTag(kvFullInfo, MINADAPTER_POOLTAG);

            ulFullInfoLength = ulFullInfoResultLength;

            kvFullInfo = (PKEY_VALUE_FULL_INFORMATION)ExAllocatePool2(
                POOL_FLAG_NON_PAGED, ulFullInfoLength, MINADAPTER_POOLTAG);
            IF_TRUE_ACTION_JUMP(kvFullInfo == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES,
                                loop_exit);

            // Try to enumerate the current value again
            ntStatus = ZwEnumerateValueKey(_hSourceKey, i, KeyValueFullInformation, kvFullInfo,
                                           ulFullInfoLength, &ulFullInfoResultLength);

            // Jump out of this loop if there are no more values
            IF_TRUE_ACTION_JUMP(ntStatus == STATUS_NO_MORE_ENTRIES, ntStatus = STATUS_SUCCESS,
                                Exit);
            IF_FAILED_JUMP(ntStatus, loop_exit);
        } else {
            IF_FAILED_JUMP(ntStatus, loop_exit);
        }

        // Allocate the key value name string
        pwstrKeyValueName = (PWSTR)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, kvFullInfo->NameLength + sizeof(WCHAR) * 2, MINADAPTER_POOLTAG);
        IF_TRUE_ACTION_JUMP(pwstrKeyValueName == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES,
                            loop_exit);

        // Copy the key value name from the full information struct
        RtlStringCbCopyNW(pwstrKeyValueName, kvFullInfo->NameLength + sizeof(WCHAR) * 2,
                          kvFullInfo->Name, kvFullInfo->NameLength);

        // Make sure the string is null terminated
        pwstrKeyValueName[(kvFullInfo->NameLength) / sizeof(WCHAR)] = 0;

        // Copy the key value name string to a UNICODE string
        RtlInitUnicodeString(&strKeyValueName, pwstrKeyValueName);

        // Write the key value from the source into the destination
        ntStatus = ZwSetValueKey(_hDestinationKey, &strKeyValueName, 0, kvFullInfo->Type,
                                 (PVOID)((PUCHAR)kvFullInfo + kvFullInfo->DataOffset),
                                 kvFullInfo->DataLength);
        IF_FAILED_JUMP(ntStatus, loop_exit);

    loop_exit:
        // Free the key value name string
        if (pwstrKeyValueName) {
            ExFreePoolWithTag(pwstrKeyValueName, MINADAPTER_POOLTAG);
            pwstrKeyValueName = NULL;
        }

        // Bail if anything failed
        IF_FAILED_JUMP(ntStatus, Exit);
    }

Exit:
    // Free the KEY_VALUE_FULL_INFORMATION structure
    if (kvFullInfo) {
        ExFreePoolWithTag(kvFullInfo, MINADAPTER_POOLTAG);
    }

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS
CopyRegistryKey(HANDLE _hSourceKey, HANDLE _hDestinationKey, BOOL _bOverwrite = FALSE)
/*++

Routine Description:

  This method recursively copies the registry values in _hSourceKey to _hDestinationKey.
  Set _bOverwrite to indicate whether the first level values are copied or not.
  Normal use is to set false for the initial call, and then all sub paths will be copied.

Return Value:

  NT status code.

--*/
{
    NTSTATUS ntStatus = STATUS_UNSUCCESSFUL;
    PKEY_BASIC_INFORMATION kBasicInfo = NULL;
    ULONG ulBasicInfoLength = 0;
    ULONG ulBasicInfoResultLength = 0;
    ULONG ulDisposition = 0;
    PWSTR pwstrKeyName = NULL;
    UNICODE_STRING strKeyName;
    OBJECT_ATTRIBUTES hCurrentSourceKeyAttributes;
    OBJECT_ATTRIBUTES hNewDestinationKeyAttributes;
    HANDLE hCurrentSourceKey = NULL;
    HANDLE hNewDestinationKey = NULL;
    PAGED_CODE();
    // Validate parameters
    IF_TRUE_ACTION_JUMP(_hSourceKey == nullptr, ntStatus = STATUS_INVALID_PARAMETER, Exit);
    IF_TRUE_ACTION_JUMP(_hDestinationKey == nullptr, ntStatus = STATUS_INVALID_PARAMETER, Exit);

    // Allocate the KEY_BASIC_INFORMATION structure
    ulBasicInfoLength = sizeof(KEY_BASIC_INFORMATION) + MAX_DEVICE_REG_KEY_LENGTH;
    kBasicInfo = (PKEY_BASIC_INFORMATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, ulBasicInfoLength,
                                                         MINADAPTER_POOLTAG);
    IF_TRUE_ACTION_JUMP(kBasicInfo == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES, Exit);

    ntStatus = STATUS_SUCCESS;
    // Iterate over each key and copy it
    for (UINT i = 0; NT_SUCCESS(ntStatus); i++) {
        // Enumerate the next key
        ntStatus = ZwEnumerateKey(_hSourceKey, i, KeyBasicInformation, kBasicInfo,
                                  ulBasicInfoLength, &ulBasicInfoResultLength);

        // Jump out of this loop if there are no more keys
        IF_TRUE_ACTION_JUMP(ntStatus == STATUS_NO_MORE_ENTRIES, ntStatus = STATUS_SUCCESS,
                            copy_values);

        // Handle incorrect buffer size
        if (ntStatus == STATUS_BUFFER_TOO_SMALL || ntStatus == STATUS_BUFFER_OVERFLOW) {
            // Free and re-allocate the KEY_BASIC_INFORMATION structure with the correct size.
            ExFreePoolWithTag(kBasicInfo, MINADAPTER_POOLTAG);
            ulBasicInfoLength = ulBasicInfoResultLength;
            kBasicInfo = (PKEY_BASIC_INFORMATION)ExAllocatePool2(
                POOL_FLAG_NON_PAGED, ulBasicInfoLength, MINADAPTER_POOLTAG);
            IF_TRUE_ACTION_JUMP(kBasicInfo == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES,
                                loop_exit);

            // Try to enumerate the current key again.
            ntStatus = ZwEnumerateKey(_hSourceKey, i, KeyBasicInformation, kBasicInfo,
                                      ulBasicInfoLength, &ulBasicInfoResultLength);

            // Jump out of this loop if there are no more keys
            IF_TRUE_ACTION_JUMP(ntStatus == STATUS_NO_MORE_ENTRIES, ntStatus = STATUS_SUCCESS,
                                copy_values);
            IF_FAILED_JUMP(ntStatus, loop_exit);
        } else {
            IF_FAILED_JUMP(ntStatus, loop_exit);
        }

        // Allocate the key name string
        pwstrKeyName = (PWSTR)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, kBasicInfo->NameLength + sizeof(WCHAR), MINADAPTER_POOLTAG);
        IF_TRUE_ACTION_JUMP(pwstrKeyName == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES,
                            loop_exit);

        // Copy the key name from the basic information struct
        RtlStringCbCopyNW(pwstrKeyName, kBasicInfo->NameLength + sizeof(WCHAR), kBasicInfo->Name,
                          kBasicInfo->NameLength);

        // Make sure the string is null terminated
        pwstrKeyName[(kBasicInfo->NameLength) / sizeof(WCHAR)] = 0;

        // Copy the key name string to a UNICODE string
        RtlInitUnicodeString(&strKeyName, pwstrKeyName);

        // Initialize attributes to open the currently enumerated source key
        InitializeObjectAttributes(&hCurrentSourceKeyAttributes, &strKeyName,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, _hSourceKey, NULL);

        // Open the currently enumerated source key
        ntStatus = ZwOpenKey(&hCurrentSourceKey, KEY_READ, &hCurrentSourceKeyAttributes);
        IF_FAILED_ACTION_JUMP(ntStatus, ZwClose(hCurrentSourceKey), loop_exit);

        // Initialize attributes to create the new destination key
        InitializeObjectAttributes(&hNewDestinationKeyAttributes, &strKeyName, OBJ_KERNEL_HANDLE,
                                   _hDestinationKey, NULL);

        // Create the key at the destination
        ntStatus = ZwCreateKey(&hNewDestinationKey, KEY_WRITE, &hNewDestinationKeyAttributes, 0,
                               NULL, REG_OPTION_NON_VOLATILE, &ulDisposition);
        IF_FAILED_ACTION_JUMP(ntStatus, ZwClose(hCurrentSourceKey), loop_exit);

        // Now copy the contents of the currently enumerated key to the destination
        ntStatus = CopyRegistryKey(hCurrentSourceKey, hNewDestinationKey, TRUE);
        IF_FAILED_JUMP(ntStatus, loop_exit);

    loop_exit:
        // Free the key name string
        if (pwstrKeyName) {
            ExFreePoolWithTag(pwstrKeyName, MINADAPTER_POOLTAG);
            pwstrKeyName = NULL;
        }

        // Close the current source key
        if (hCurrentSourceKey) {
            ZwClose(hCurrentSourceKey);
        }

        // Close the new destination key
        if (hNewDestinationKey) {
            ZwClose(hNewDestinationKey);
        }

        // Bail if anything failed
        IF_FAILED_JUMP(ntStatus, Exit);
    }

copy_values:
    // Copy the values
    if (_bOverwrite) {
        ntStatus = CopyRegistryValues(_hSourceKey, _hDestinationKey);
        IF_FAILED_JUMP(ntStatus, Exit);
    }

Exit:
    // Free the basic information structure
    if (kBasicInfo) {
        ExFreePoolWithTag(kBasicInfo, MINADAPTER_POOLTAG);
    }
    return ntStatus;
}

// Dynamic interfaces need the INF's KS proxy and initial display name as well
// as its EP/FX subkeys. Copy only these two root values, and leave any existing
// dynamic-interface values intact; never copy interface state or user settings.
#pragma code_seg("PAGE")
static NTSTATUS CopyInitialInterfaceValue(HANDLE source, HANDLE destination, PCWSTR valueName) {
    PAGED_CODE();
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, valueName);
    ULONG bytes{};
    NTSTATUS status =
        ZwQueryValueKey(destination, &name, KeyValuePartialInformation, nullptr, 0, &bytes);
    if (status == STATUS_BUFFER_TOO_SMALL || status == STATUS_BUFFER_OVERFLOW || NT_SUCCESS(status))
        return STATUS_SUCCESS;
    if (status != STATUS_OBJECT_NAME_NOT_FOUND)
        return status;
    alignas(ULONG) UCHAR storage[512]{};
    status = ZwQueryValueKey(source, &name, KeyValuePartialInformation, storage, sizeof(storage),
                             &bytes);
    if (!NT_SUCCESS(status))
        return status;
    const auto* value = reinterpret_cast<const KEY_VALUE_PARTIAL_INFORMATION*>(storage);
    if (value->Type != REG_SZ || value->DataLength < sizeof(WCHAR) ||
        value->DataLength > sizeof(storage) - FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) ||
        value->DataLength % sizeof(WCHAR) ||
        reinterpret_cast<const WCHAR*>(value->Data)[value->DataLength / sizeof(WCHAR) - 1] != 0)
        return STATUS_INVALID_PARAMETER;
    return ZwSetValueKey(destination, &name, 0, REG_SZ, const_cast<UCHAR*>(value->Data),
                         value->DataLength);
}

#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::MigrateDeviceInterfaceTemplateParameters(_In_ PUNICODE_STRING SymbolicLinkName,
                                                         _In_opt_ PCWSTR TemplateReferenceString)
/*++

Routine Description:

  This method copies all of the properties from the template interface,
  which is specified in the inf, to the actual interface being used which
  may be dynamically generated at run time. This allows for a driver 
  to reuse a single inf entry for multiple audio endpoints. The primary 
  purpose for this is to allow for sideband audio endpoints to dynamically
  generate the reference string at run time, tied to the peripheral connected,
  while still having a simple static inf entry for setting up apo's or other 
  parameters.

  For example, if you have an interface in your inf defined with reference string
  "SpeakerWave". At runtime you could generate "SpeakerWave-1234ABCDE", and specify
  "SpeakerWave" as the template name. When "SpeakerWave-1234ABCDE" is installed
  we will copy all of the parameters that were specified in the inf for "SpeakerWave"
  over to "SpeakerWave-1234ABCDE". You simply need to specify "SpeakerWave" as the
  "TemplateName" in the ENDPOINT_MINIPAIRS.

  By default, the first level of registry keys are not copied. Only the 2nd level and
  deeper are copied. This way the friendly name and other PNP properties will not
  be modified, but the EP and FX properties will be copied.
  
Return Value:

  NT status code.

--*/

{
    NTSTATUS ntStatus = STATUS_SUCCESS;
    HANDLE hDeviceInterfaceParametersKey(NULL);
    HANDLE hTemplateDeviceInterfaceParametersKey(NULL);
    UNICODE_STRING TemplateSymbolicLinkName;
    UNICODE_STRING referenceString;

    PAGED_CODE();

    RtlInitUnicodeString(&TemplateSymbolicLinkName, NULL);
    RtlInitUnicodeString(&referenceString, TemplateReferenceString);

    //
    // Register an audio interface if not already present for the template interface, so we can access
    // the registry path. If it's already registered, this simply returns the symbolic link name.
    // No need to unregister it (there is no mechanism to), and we'll never make it active.
    //
    ntStatus = IoRegisterDeviceInterface(GetPhysicalDeviceObject(), &KSCATEGORY_AUDIO,
                                         &referenceString, &TemplateSymbolicLinkName);

    // Open the template device interface's registry key path
    ntStatus = IoOpenDeviceInterfaceRegistryKey(&TemplateSymbolicLinkName, GENERIC_READ,
                                                &hTemplateDeviceInterfaceParametersKey);
    IF_FAILED_JUMP(ntStatus, Exit);

    // Open the new device interface's registry key path that we plan to activate
    ntStatus = IoOpenDeviceInterfaceRegistryKey(SymbolicLinkName, GENERIC_READ | GENERIC_WRITE,
                                                &hDeviceInterfaceParametersKey);
    IF_FAILED_JUMP(ntStatus, Exit);

    ntStatus = CopyInitialInterfaceValue(hTemplateDeviceInterfaceParametersKey,
                                         hDeviceInterfaceParametersKey, L"CLSID");
    IF_FAILED_JUMP(ntStatus, Exit);
    ntStatus = CopyInitialInterfaceValue(hTemplateDeviceInterfaceParametersKey,
                                         hDeviceInterfaceParametersKey, L"FriendlyName");
    IF_FAILED_JUMP(ntStatus, Exit);

    // Copy the template device parameters key to the device interface key
    ntStatus =
        CopyRegistryKey(hTemplateDeviceInterfaceParametersKey, hDeviceInterfaceParametersKey);
    IF_FAILED_JUMP(ntStatus, Exit);

Exit:
    RtlFreeUnicodeString(&TemplateSymbolicLinkName);

    if (hTemplateDeviceInterfaceParametersKey) {
        ZwClose(hTemplateDeviceInterfaceParametersKey);
    }

    if (hDeviceInterfaceParametersKey) {
        ZwClose(hDeviceInterfaceParametersKey);
    }

    return ntStatus;
}
