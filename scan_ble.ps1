$null = [Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher, Windows.Devices.Bluetooth, ContentType = WindowsRuntime]
$watcher = New-Object Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher
$watcher.ScanningMode = 1 # Active

$sub = Register-ObjectEvent -InputObject $watcher -EventName Received -Action {
    $e = $Event.SourceEventArgs
    $name = $e.Advertisement.LocalName
    $addr = ('{0:X12}' -f $e.BluetoothAddress)
    Write-Host "FOUND: $name ($addr)"
}

$watcher.Start()
Start-Sleep -Seconds 4
$watcher.Stop()
Unregister-Event -SourceIdentifier $sub.Name
