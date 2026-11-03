# SPDX-License-Identifier: GPL-2.0-or-later
# Run in Windows PowerShell. No administrator privileges or writes required.
[CmdletBinding(DefaultParameterSetName = 'Drive')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Drive')]
    [ValidatePattern('^[A-Za-z]:?$')][string]$DriveLetter,
    [Parameter(Mandatory = $true, ParameterSetName = 'Disk')]
    [ValidateRange(0, 65535)][int]$DiskNumber,
    [Parameter(Mandatory = $true, ParameterSetName = 'List')][switch]$ListUsb
)
$ErrorActionPreference = 'Stop'
if ($ListUsb) {
    Get-Disk | Where-Object BusType -eq 'USB' | Select-Object Number, FriendlyName, SerialNumber, Size
    return
}
if ($PSCmdlet.ParameterSetName -eq 'Drive') {
    $partition = Get-Partition -DriveLetter $DriveLetter.Substring(0, 1)
    $DiskNumber = $partition.DiskNumber
}
$disk = Get-CimInstance Win32_DiskDrive | Where-Object Index -eq $DiskNumber
if (-not $disk) { throw "Disk $DiskNumber not found" }
$id = $disk.PNPDeviceID
$seen = @{}
Write-Output "Disk $DiskNumber ($($disk.Model)): $id"
while ($id -and -not $seen.ContainsKey($id)) {
    $seen[$id] = $true
    $compatible = Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_CompatibleIds -ErrorAction SilentlyContinue
    if ($id.StartsWith('PCI\') -and ($compatible.Data | Where-Object { $_ -match '^PCI\\CC_(01|0C03)' })) {
        $bus = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_BusNumber).Data
        $address = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Address).Data
        $device = ($address -shr 16) -band 0xffff
        $function = $address -band 0xffff
        if ($bus -gt 255 -or $device -gt 31 -or $function -gt 7) { throw 'Controller is outside segment-0 BDF limits' }
        $bdf = '{0:x2}:{1:x2}.{2:x}' -f $bus, $device, $function
        Write-Output "Controller: $id"
        Write-Output "Windows BDF: $bdf (bus $bus decimal)"
        Write-Output "Candidate: vmm_media=$bdf"
        Write-Output 'Confirm IDs and bridge ancestry with vmm_media=list; UEFI bus numbering may differ.'
        return
    }
    $parent = Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Parent -ErrorAction SilentlyContinue
    $id = $parent.Data
}
throw 'No storage/USB PCI controller found in the disk parent chain'
