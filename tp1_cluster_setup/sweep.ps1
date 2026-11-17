$alive = @()
100..200 | ForEach-Object {
    $ip = "10.77.77.$_"
    $ping = ping.exe $ip -n 1 -w 80
    if ($ping -match "TTL=") {
        $alive += $ip
        Write-Output "$ip ESTA ACTIVO"
    }
}
if ($alive.Count -eq 0) {
    Write-Output "NINGUN NODO RESPONDE EN EL RANGO 100-200"
}
