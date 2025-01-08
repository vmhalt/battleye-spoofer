#include "defines.h"

NTSTATUS disk_handler(PDEVICE_OBJECT dev, PIRP irp) {
    auto stack = IoGetCurrentIrpStackLocation(irp);
    auto len = stack->Parameters.DeviceIoControl.InputBufferLength;
    auto code = stack->Parameters.DeviceIoControl.IoControlCode;

    if (len > 5) {
        switch (code) {
            case 0x4D004:   // IOCTL_SCSI_PASS_THROUGH
            case 0x2D1400:  // IOCTL_STORAGE_QUERY_PROPERTY
            case 0x4D014:   // IOCTL_SCSI_PASS_THROUGH_DIRECT
            {
                irp->IoStatus.Status = STATUS_ACCESS_DENIED;
                IoCompleteRequest(irp, IO_NO_INCREMENT);
                return STATUS_ACCESS_DENIED;
            }
            default:
                return orig_disk(dev, irp);
        }
    }
    return orig_disk(dev, irp);
}

NTSTATUS tpm_handler(PDEVICE_OBJECT dev, PIRP irp) {
    auto stack = IoGetCurrentIrpStackLocation(irp);
    auto len = stack->Parameters.DeviceIoControl.InputBufferLength;
    auto code = stack->Parameters.DeviceIoControl.IoControlCode;

    switch (code) {
        case 0x22C004:  // TPM_READ_DATA
        case 0x22C010:  // TPM_GET_VERSION
        case 0x22C014:  // TPM_GET_CAPABILITY
        case 0x22C01C:  // TPM_SEND_COMMAND
        case 0x22C00C:  // TPM_EXECUTE_COMMAND
        case 0x22C194:  // TPM_GET_INFO
        {
            irp->IoStatus.Information = 0;
            irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        default:
            return orig_tpm(dev, irp);
    }
}

extern "C" NTSTATUS entry() {
    UNICODE_STRING name, tpm_name;
    RtlInitUnicodeString(&name, L"\\Driver\\Disk");
    RtlInitUnicodeString(&tpm_name, L"\\Driver\\TPM");

    PDRIVER_OBJECT disk = nullptr;
    PDRIVER_OBJECT tpm = nullptr;

    ObReferenceObjectByName(&name, OBJ_CASE_INSENSITIVE, 0, 0,
        *IoDriverObjectType, KernelMode, 0, (PVOID*)&disk);

    if (disk) {
        orig_disk = disk->MajorFunction[IRP_MJ_DEVICE_CONTROL];
        disk->MajorFunction[IRP_MJ_DEVICE_CONTROL] = disk_handler;
        ObDereferenceObject(disk);
    }

    ObReferenceObjectByName(&tpm_name, OBJ_CASE_INSENSITIVE, 0, 0,
        *IoDriverObjectType, KernelMode, 0, (PVOID*)&tpm);

    if (tpm) {
        orig_tpm = tpm->MajorFunction[IRP_MJ_DEVICE_CONTROL];
        tpm->MajorFunction[IRP_MJ_DEVICE_CONTROL] = tpm_handler;
        ObDereferenceObject(tpm);
    }

    return STATUS_SUCCESS;
}