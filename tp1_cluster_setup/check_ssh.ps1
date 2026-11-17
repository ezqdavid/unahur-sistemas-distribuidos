$client = New-Object System.Net.Sockets.TcpClient
try {
    $async = $client.BeginConnect('192.168.1.190', 22, $null, $null)
    $success = $async.AsyncWaitHandle.WaitOne(2000)
    if ($success -and $client.Connected) {
        Write-Output "SSH_PORT_22_IS_OPEN"
    } else {
        Write-Output "SSH_PORT_22_TIMED_OUT"
    }
} catch {
    Write-Output "SSH_ERROR: $_"
} finally {
    $client.Close()
}
