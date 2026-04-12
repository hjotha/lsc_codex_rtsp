param(
    [Parameter(Mandatory = $true)]
    [string]$CameraHost,

    [int]$Port = 88,

    [string]$Path = "videoMain",

    [ValidateSet("video", "audio")]
    [string]$Kind = "video",

    [int]$ClientPort = 50020,

    [int]$Packets = 5,

    [int]$TimeoutMs = 4000
)

$ErrorActionPreference = "Stop"

function Read-Exact {
    param(
        [Parameter(Mandatory = $true)]
        [System.Net.Sockets.NetworkStream]$Stream,

        [Parameter(Mandatory = $true)]
        [int]$Count
    )

    $buffer = New-Object byte[] $Count
    $offset = 0
    while ($offset -lt $Count) {
        $read = $Stream.Read($buffer, $offset, $Count - $offset)
        if ($read -le 0) {
            throw "connection closed while reading RTSP body"
        }
        $offset += $read
    }
    return $buffer
}

function Read-RtspResponse {
    param(
        [Parameter(Mandatory = $true)]
        [System.Net.Sockets.NetworkStream]$Stream
    )

    $buffer = New-Object byte[] 4096
    $accum = New-Object System.Collections.Generic.List[byte]
    $headerEnd = -1

    while ($headerEnd -lt 0) {
        $read = $Stream.Read($buffer, 0, $buffer.Length)
        if ($read -le 0) {
            throw "connection closed while reading RTSP headers"
        }
        for ($i = 0; $i -lt $read; $i++) {
            [void]$accum.Add($buffer[$i])
        }
        $text = [System.Text.Encoding]::ASCII.GetString($accum.ToArray())
        $headerEnd = $text.IndexOf("`r`n`r`n")
    }

    $all = $accum.ToArray()
    $headerBytesLen = $headerEnd + 4
    $headerText = [System.Text.Encoding]::ASCII.GetString($all, 0, $headerBytesLen)
    $lines = $headerText.TrimEnd("`r", "`n").Split("`r`n")
    $statusLine = $lines[0]
    $headers = @{}

    foreach ($line in $lines[1..($lines.Length - 1)]) {
        if ($line -match '^[^:]+:') {
            $parts = $line.Split(':', 2)
            $headers[$parts[0].Trim().ToLowerInvariant()] = $parts[1].Trim()
        }
    }

    $contentLength = 0
    if ($headers.ContainsKey("content-length")) {
        $contentLength = [int]$headers["content-length"]
    }

    $bodyRemainderLen = $all.Length - $headerBytesLen
    $body = New-Object byte[] $contentLength
    if ($contentLength -gt 0) {
        if ($bodyRemainderLen -gt 0) {
            [Array]::Copy($all, $headerBytesLen, $body, 0, [Math]::Min($bodyRemainderLen, $contentLength))
        }
        if ($bodyRemainderLen -lt $contentLength) {
            $extra = Read-Exact -Stream $Stream -Count ($contentLength - $bodyRemainderLen)
            [Array]::Copy($extra, 0, $body, $bodyRemainderLen, $extra.Length)
        }
    }

    [PSCustomObject]@{
        StatusLine = $statusLine
        Headers    = $headers
        Body       = $body
    }
}

function Send-Rtsp {
    param(
        [Parameter(Mandatory = $true)]
        [System.Net.Sockets.NetworkStream]$Stream,

        [Parameter(Mandatory = $true)]
        [int]$CSeq,

        [Parameter(Mandatory = $true)]
        [string]$Method,

        [Parameter(Mandatory = $true)]
        [string]$Uri,

        [string[]]$ExtraHeaders = @()
    )

    $lines = @(
        "$Method $Uri RTSP/1.0"
        "CSeq: $CSeq"
        "User-Agent: codex-win-probe"
    ) + $ExtraHeaders + @("", "")
    $payload = [System.Text.Encoding]::ASCII.GetBytes(($lines -join "`r`n"))
    $Stream.Write($payload, 0, $payload.Length)
    $resp = Read-RtspResponse -Stream $Stream

    Write-Output $resp.StatusLine
    foreach ($entry in ($resp.Headers.GetEnumerator() | Sort-Object Name)) {
        Write-Output ("{0}: {1}" -f $entry.Key, $entry.Value)
    }
    if ($resp.Body.Length -gt 0) {
        Write-Output ""
        Write-Output ([System.Text.Encoding]::ASCII.GetString($resp.Body))
    }
    Write-Output ""

    return $resp
}

function Get-TrackUri {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BaseUri,

        [Parameter(Mandatory = $true)]
        [string]$Sdp,

        [Parameter(Mandatory = $true)]
        [string]$MediaKind
    )

    $currentKind = $null
    foreach ($line in ($Sdp -split "`n")) {
        $trimmed = $line.Trim("`r")
        if ($trimmed.StartsWith("m=")) {
            if ($trimmed.StartsWith("m=video ")) {
                $currentKind = "video"
            } elseif ($trimmed.StartsWith("m=audio ")) {
                $currentKind = "audio"
            } else {
                $currentKind = $null
            }
            continue
        }

        if ($currentKind -ne $MediaKind) {
            continue
        }
        if (-not $trimmed.StartsWith("a=control:")) {
            continue
        }

        $control = $trimmed.Substring("a=control:".Length)
        if ($control.StartsWith("rtsp://")) {
            return $control
        }
        return ($BaseUri.TrimEnd("/") + "/" + $control.TrimStart("/"))
    }

    throw "could not find $MediaKind control URI in SDP"
}

function Describe-RtpPacket {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Packet
    )

    if ($Packet.Length -lt 12) {
        return "short packet ($($Packet.Length) bytes)"
    }

    $version = $Packet[0] -shr 6
    $marker = ($Packet[1] -shr 7) -band 0x1
    $payloadType = $Packet[1] -band 0x7f
    $seq = [System.BitConverter]::ToUInt16([byte[]]@($Packet[3], $Packet[2]), 0)
    $ts = [System.BitConverter]::ToUInt32([byte[]]@($Packet[7], $Packet[6], $Packet[5], $Packet[4]), 0)
    $ssrc = [System.BitConverter]::ToUInt32([byte[]]@($Packet[11], $Packet[10], $Packet[9], $Packet[8]), 0)
    return "len=$($Packet.Length) version=$version pt=$payloadType marker=$marker seq=$seq ts=$ts ssrc=0x$($ssrc.ToString('x8'))"
}

$baseUri = "rtsp://$CameraHost`:$Port/$Path"
$tcp = [System.Net.Sockets.TcpClient]::new()
$tcp.ReceiveTimeout = $TimeoutMs
$tcp.SendTimeout = $TimeoutMs
$tcp.Connect($CameraHost, $Port)
$stream = $tcp.GetStream()
$stream.ReadTimeout = $TimeoutMs
$stream.WriteTimeout = $TimeoutMs

$rtpClient = [System.Net.Sockets.UdpClient]::new($ClientPort)
$rtcpClient = [System.Net.Sockets.UdpClient]::new($ClientPort + 1)
$rtpClient.Client.ReceiveTimeout = $TimeoutMs

try {
    [void](Send-Rtsp -Stream $stream -CSeq 1 -Method "OPTIONS" -Uri $baseUri)
    $describe = Send-Rtsp -Stream $stream -CSeq 2 -Method "DESCRIBE" -Uri $baseUri -ExtraHeaders @("Accept: application/sdp")
    $sdp = [System.Text.Encoding]::ASCII.GetString($describe.Body)
    $trackUri = Get-TrackUri -BaseUri $baseUri -Sdp $sdp -MediaKind $Kind
    Write-Output "Selected $Kind track URI: $trackUri"
    Write-Output ""

    $setup = Send-Rtsp -Stream $stream -CSeq 3 -Method "SETUP" -Uri $trackUri -ExtraHeaders @("Transport: RTP/AVP;unicast;client_port=$ClientPort-$($ClientPort + 1)")
    $session = ($setup.Headers["session"] -split ";")[0].Trim()
    [void](Send-Rtsp -Stream $stream -CSeq 4 -Method "PLAY" -Uri $baseUri -ExtraHeaders @("Session: $session", "Range: npt=0.000-"))

    Write-Output "Waiting for up to $Packets RTP packets on UDP $ClientPort"
    for ($i = 1; $i -le $Packets; $i++) {
        $remote = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
        $packet = $rtpClient.Receive([ref]$remote)
        Write-Output ("RTP[{0}] from {1}:{2} {3}" -f $i, $remote.Address, $remote.Port, (Describe-RtpPacket -Packet $packet))
    }
} finally {
    $rtpClient.Close()
    $rtcpClient.Close()
    $stream.Close()
    $tcp.Close()
}
