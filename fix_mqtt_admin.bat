@echo off
echo Configuring Mosquitto MQTT Broker to accept Wi-Fi ESP32 connections...
powershell -ExecutionPolicy Bypass -Command "$conf='C:\Program Files\mosquitto\mosquitto.conf'; $txt=Get-Content $conf -Raw; if($txt -notmatch '(?m)^listener 1883 0\.0\.0\.0'){Add-Content -Path $conf -Value \"`nlistener 1883 0.0.0.0\"}; if($txt -notmatch '(?m)^allow_anonymous true'){Add-Content -Path $conf -Value 'allow_anonymous true'}; if(-not (Get-NetFirewallRule -DisplayName 'MQTT 1883' -ErrorAction SilentlyContinue)){New-NetFirewallRule -DisplayName 'MQTT 1883' -Direction Inbound -Protocol TCP -LocalPort 1883 -Action Allow | Out-Null}; Restart-Service mosquitto; Start-Sleep -Seconds 2; netstat -ano | Select-String ':1883'"
echo.
echo ==================================================================
echo DONE! Look above for "0.0.0.0:1883 ... LISTENING"
echo Your ESP32 will auto-connect within 5 seconds!
echo ==================================================================
pause
