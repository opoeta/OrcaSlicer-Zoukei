// Ported from Creality Print (src/slic3r/GUI/print_manage/PrinterMgr.hpp,
// CrealityOfficial/CrealityPrint@59ae8cbb0437dcc7236e27433ecd4aeb52be0244, AGPL-3.0).
#ifndef slic3r_CrealityDevice_DeviceStore_hpp_
#define slic3r_CrealityDevice_DeviceStore_hpp_
#include <map>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace Slic3r { namespace GUI { namespace CrealityDevice {
    class DeviceStore {
    public:
        struct Data
        {
            int connectType;
            std::string model;
            std::string mac;
            std::string address;
            std::uint64_t addressRevision = 0;
            std::string name;
            std::string deviceUI;
            
            bool oldPrinter = false; // Legacy printer such as D3Pro
            bool secureConnection = false;
            int wssPort = 0;
            int videoPort = 0;
            int moonrakerPort = 0;   // Moonraker status port
            int fluiddPort = 0;
            int mainsailPort = 0;

            std::string apiKey;
            int hostType;
            std::string caFile;
            bool ignoreCertRevocation = false;
        };

        struct ConnectionPatch
        {
            std::optional<bool> secureConnection;
            std::optional<int> wssPort;
            std::optional<int> videoPort;
        };

        struct UpdatePatch
        {
            std::optional<std::string> lookupAddress;
            std::optional<std::string> mac;
            std::optional<std::string> address;
            std::optional<std::string> model;
            std::optional<int> connectType;
            ConnectionPatch connection;
        };

        enum class UpdateDeviceOutcome
        {
            Updated,
            AlreadyApplied,
            StaleSource,
            SourceNotFound,
            DuplicateMacConflict,
            AddressConflict,
            PersistenceFailed,
            InvalidRequest
        };

        struct AddressUpdateExpectation
        {
            std::string expectedAddress;
            std::uint64_t expectedAddressRevision = 0;
        };

        struct UpdateDeviceResult
        {
            UpdateDeviceOutcome outcome = UpdateDeviceOutcome::InvalidRequest;
            bool changed = false;
            std::optional<Data> device;
            std::string error;
        };

        enum class DeviceOwnershipBatchOutcome
        {
            Reconciled,
            AlreadyApplied,
            StaleSource,
            PersistenceFailed,
            InvalidRequest
        };

        struct DeviceOwnershipExpectedRecord
        {
            std::size_t rawIndex = 0;
            std::string group;
            std::string mac;
            std::string address;
            std::uint64_t addressRevision = 0;
        };

        struct DeviceOwnershipProbe
        {
            std::string address;
            bool ok = false;
            std::string observedMac;
            std::string model;
            bool secureConnection = false;
            bool allowSecureDowngrade = false;
            int wssPort = 0;
            int videoPort = 0;
        };

        struct DeviceOwnershipBatchRequest
        {
            std::vector<DeviceOwnershipExpectedRecord> expectedRecords;
            std::vector<DeviceOwnershipProbe> probes;
        };

        struct ModernLanDeviceSnapshot
        {
            std::size_t rawIndex = 0;
            std::string group;
            Data data;
        };

        struct DeviceOwnershipBatchResult
        {
            DeviceOwnershipBatchOutcome outcome = DeviceOwnershipBatchOutcome::InvalidRequest;
            bool changed = false;
            std::vector<ModernLanDeviceSnapshot> snapshot;
            std::string error;
        };
    public:
        DeviceStore();
        ~DeviceStore();
        // Reads the device file; false when there is none (or it was unreadable).
        bool Load();
        // Replaces the whole list (used to seed it) and saves it.
        void Import(const nlohmann::json& data);
        void Save();
        void AddDevice(std::string group, Data& data);
        void RemoveDevice(std::string name);
        void EditDeiveName(std::string name, std::string nameNew);
        void UpdateDevice(std::string mac, Data& data);
        bool UpdateDevicePatch(const std::string& lookupMac, const UpdatePatch& patch, bool allowSecureDowngrade = false);
        UpdateDeviceResult UpdateDevicePatchCas(const std::string& lookupMac,
                                                const UpdatePatch& patch,
                                                const AddressUpdateExpectation& expectation,
                                                bool allowSecureDowngrade = false);
        DeviceOwnershipBatchResult ReconcileDeviceOwnershipBatch(
            const DeviceOwnershipBatchRequest& request);
        void AddGroup(std::string name, bool is_save=true);
        void RemoveGroup(std::string name);
        void EditGroupName(std::string name, std::string nameNew);
        void remove2FirstGroup(std::string group);
        void move2Group(std::string originGroup, std::string targetGroup, std::string address);
        void sortGroup(std::vector<std::string> order = {"new group2", "22", "new group"});
        void SetMergeState(bool state);
        bool IsMergeState();
        nlohmann::json GetData();
        std::optional<Data> FindByAddress(const std::string& address) const;
        std::optional<Data> FindByMac(const std::string& mac) const;
        void Get(std::map<std::string, std::vector<DeviceStore::Data>>& store, std::vector<std::string>& order);
        bool IsGroupExist(std::string name);
        bool IsPrinterExist(std::string mac);
        void SetCurrentDevice(std::string mac);
        std::string GetCurrentDevice();
    private:
        void Normalize();
        struct priv;
        std::unique_ptr<priv> p;
    public:
        static DeviceStore& Ins();
    };

}}} // namespace Slic3r::GUI::CrealityDevice

#endif /* slic3r_CrealityDevice_DeviceStore_hpp_ */
