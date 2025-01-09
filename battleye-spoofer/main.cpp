#include "defines.h"

NTSTATUS disk_handler(PDEVICE_OBJECT dev, PIRP irp) {
    auto stack = IoGetCurrentIrpStackLocation(irp);
    auto code = stack->Parameters.DeviceIoControl.IoControlCode;
    if (stack->Parameters.DeviceIoControl.InputBufferLength > 5) {
        switch (code) {
            case 0x4D004:   // IOCTL_SCSI_PASS_THROUGH
            case 0x2D1400:  // IOCTL_STORAGE_QUERY_PROPERTY
            case 0x4D014:   // IOCTL_SCSI_PASS_THROUGH_DIRECT
            case 0x70050:   // IOCTL_DISK_GET_DRIVE_GEOMETRY
            case 0x70140:   // IOCTL_DISK_GET_PARTITION_INFO
            case 0x70000:   // IOCTL_DISK_GET_DRIVE_LAYOUT
            case 0x2D1080:  // IOCTL_STORAGE_GET_DEVICE_NUMBER
            case 0x2D5100:  // IOCTL_STORAGE_GET_HOTPLUG_INFO
            case 0x2D1440:  // IOCTL_STORAGE_GET_MEDIA_TYPES_EX
            case 0x2D0C14:  // IOCTL_STORAGE_GET_MEDIA_SERIAL_NUMBER
                irp->IoStatus.Information = 0;
                irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
                IoCompleteRequest(irp, IO_NO_INCREMENT);
                return STATUS_DEVICE_NOT_CONNECTED;
            case 0x74080:   // SMART_GET_VERSION
            case 0x74084:   // SMART_SEND_DRIVE_COMMAND
            case 0x740C8:   // SMART_RCV_DRIVE_DATA
                irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
                IoCompleteRequest(irp, IO_NO_INCREMENT);
                return STATUS_INVALID_DEVICE_REQUEST;
        }
    }
    return orig_disk(dev, irp);
}

NTSTATUS tpm_handler(PDEVICE_OBJECT dev, PIRP irp) {
    auto code = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.IoControlCode;
    switch (code) {
        case 0x22C004:  // TPM_READ_DATA
        case 0x22C010:  // TPM_GET_VERSION
        case 0x22C014:  // TPM_GET_CAPABILITY
        case 0x22C01C:  // TPM_SEND_COMMAND
        case 0x22C00C:  // TPM_EXECUTE_COMMAND
        case 0x22C194:  // TPM_GET_INFO
            irp->IoStatus.Information = 0;
            irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return STATUS_DEVICE_NOT_READY;
        default:
            return orig_tpm(dev, irp);
    }
}

NTSTATUS monitor_handler(PDEVICE_OBJECT dev, PIRP irp) {
    auto stack = IoGetCurrentIrpStackLocation(irp);
    if (stack->MajorFunction == IRP_MJ_DEVICE_CONTROL || 
        stack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL) {
        irp->IoStatus.Information = 0;
        irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    return orig_monitor(dev, irp);
}

NTSTATUS null_smbios() {
    ULONG size = 0;
    ZwQuerySystemInformation(SystemModuleInformation, &size, 0, &size);
    if (auto list = static_cast<PSYSTEM_MODULE_INFORMATION>(ExAllocatePoolWithTag(NonPagedPool, size, 'skid'))) {
        if (NT_SUCCESS(ZwQuerySystemInformation(SystemModuleInformation, list, size, nullptr))) {
            for (ULONG i = 0; i < list->ulModuleCount; i++) {
                if (strstr(list->Modules[i].ImageName, "ntoskrnl.exe")) {
                    auto base = list->Modules[i].Base;
                    auto headers = reinterpret_cast<PIMAGE_NT_HEADERS>(static_cast<char*>(base) + static_cast<PIMAGE_DOS_HEADER>(base)->e_lfanew);
                    auto sections = IMAGE_FIRST_SECTION(headers);
                    for (ULONG j = 0; j < headers->FileHeader.NumberOfSections; j++) {
                        if ('EGAP' == *reinterpret_cast<PINT>(sections[j].Name) || memcmp(sections[j].Name, ".text", 5) == 0) {
                            auto data = static_cast<char*>(base) + sections[j].VirtualAddress;
                            for (int k = 0; k <= sections[j].Misc.VirtualSize - 14; k++) {
                                if ((data[k+0] == '\x48' && data[k+1] == '\x8B' && data[k+2] == '\x0D' &&
                                     data[k+7] == '\x48' && data[k+8] == '\x85' && data[k+9] == '\xC9' &&
                                     data[k+10] == '\x74' && data[k+12] == '\x8B' && data[k+13] == '\x15')) {
                                    auto addr = reinterpret_cast<PPHYSICAL_ADDRESS>(&data[k] + 7 + *reinterpret_cast<int*>(&data[k] + 3));
                                    if (auto mapped = MmMapIoSpace(*addr, 0x1000, MmNonCached)) {
                                        for (auto curr = static_cast<SMBIOS_HEADER*>(mapped); curr->Type != 127;) {
                                            if (curr->Type == 1 || curr->Type == 2) { // Nulls out the SMBIOS Type 1 and Type 2 tables
                                                RtlZeroMemory(reinterpret_cast<char*>(curr) + sizeof(SMBIOS_HEADER), curr->Length - sizeof(SMBIOS_HEADER));
                                                auto str = reinterpret_cast<char*>(curr) + curr->Length;
                                                while (*str || *(str + 1)) {
                                                    RtlZeroMemory(str, strlen(str));
                                                    str += strlen(str) + 1;
                                                }
                                            }
                                            auto end = reinterpret_cast<char*>(curr) + curr->Length;
                                            while (0 != (*end | *(end + 1))) end++;
                                            curr = reinterpret_cast<SMBIOS_HEADER*>(end + 2);
                                        }
                                        MmUnmapIoSpace(mapped, 0x1000);
                                        ExFreePool(list);
                                        return STATUS_SUCCESS;
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }
        ExFreePool(list);
    }
    return STATUS_UNSUCCESSFUL;
}

extern "C" NTSTATUS entry() {
    null_smbios();

    UNICODE_STRING names[3];
    PDRIVER_OBJECT objs[3] = {nullptr};
    RtlInitUnicodeString(&names[0], L"\\Driver\\Disk");
    RtlInitUnicodeString(&names[1], L"\\Driver\\TPM");
    RtlInitUnicodeString(&names[2], L"\\Driver\\Monitor");

    for (int i = 0; i < 3; i++) {
        if (NT_SUCCESS(ObReferenceObjectByName(&names[i], OBJ_CASE_INSENSITIVE, 0, 0,
            *IoDriverObjectType, KernelMode, 0, (PVOID*)&objs[i]))) {
            if (i == 0) {
                orig_disk = objs[i]->MajorFunction[IRP_MJ_DEVICE_CONTROL];
                objs[i]->MajorFunction[IRP_MJ_DEVICE_CONTROL] = disk_handler;
            } else if (i == 1) {
                orig_tpm = objs[i]->MajorFunction[IRP_MJ_DEVICE_CONTROL];
                objs[i]->MajorFunction[IRP_MJ_DEVICE_CONTROL] = tpm_handler;
            } else {
                orig_monitor = objs[i]->MajorFunction[IRP_MJ_DEVICE_CONTROL];
                objs[i]->MajorFunction[IRP_MJ_DEVICE_CONTROL] = monitor_handler;
            }
            ObDereferenceObject(objs[i]);
        }
    }
    return STATUS_SUCCESS;
}