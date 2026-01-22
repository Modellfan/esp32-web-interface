# Supabase Post example

## Create the table as shown in the image below

![CREATE](./images/create.png)

## To simplify testing, the server role api-key is used. However, this should not be used in actual production

![API-KEY](./images/password.png)

## Test log

![testlog](./images/testlog.png)

## Test Commands

```bash

« Wait for the modem to register with the network.AT+CEREG?
AT+CEREG?

+CEREG: 0,1

OK
Online registration successful

Registration Status:1
AT+CPSI?
AT+CPSI?

+CPSI: LTE,Online,460-11,0x775C,117004585,237,EUTRAN-BAND1,100,5,0,20,50,50,0

OK
Inquiring UE system information:LTE,Online,460-11,0x775C,117004585,237,EUTRAN-BAND1,100,5,0,20,50,50,0
Connecting to network with APN:
Use IPv6 access point:false
AT+NETCLOSE
AT+NETCLOSE

OK

+NETCLOSE: 0AT+CSOCKSETPN=1,1

AT+CSOCKSETPN=1,1

OK
AT+CIPCFG="CID",1
AT+CIPCFG="CID",1

OK
AT+NETOPEN
AT+NETOPEN

OK

+NETOPEN: 0AT+IPADDR

AT+IPADDR

+IPADDR: 10.3.82.86

OK
Network IP:10.3.82.86
AT+SIMCOMATI
AT+SIMCOMATI

Manufacturer: INCORPORATED
Model: A7670E-FASE
Revision: A011B09A7670M7_F
A7670M7_B09V01_250619
QCN:
IMEI: xxxxxxxxxx
MEID:
+GCAP: +CGSM,+FCLASS,+DS
DeviceInfo:

OK
AT+SIMCOMATI

Manufacturer: INCORPORATED
Model: A7670E-FASE
Revision: A011B09A7670M7_F
A7670M7_B09V01_250619
QCN:
IMEI: xxxxxxxxxx
MEID:
+GCAP: +CGSM,+FCLASS,+DS
DeviceInfo:

OK

Please make sure you are using the latest released version of the firmware. Find the latest version here: https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series#modem-firmware-upgrade-guide
If you still have problems with the latest firmware, please open an issue. Otherwise, please do not create meaningless issues.
AT+HTTPTERM
AT+HTTPTERM

OK
AT+HTTPINIT
AT+HTTPINIT

OK
AT+CSSLCFG="enableSNI",0,1
AT+CSSLCFG="enableSNI",0,1

OK
HTTPS initialized successfully
AT+CSSLCFG="sslversion",0,4
AT+CSSLCFG="sslversion",0,4

OK
AT+HTTPPARA="URL","https://xxxxxxxxxxx.supabase.co/rest/v1/locations"
AT+HTTPPARA="URL","https://xxxxxxxxxxx.supabase.co/rest/v1/locations"

OK
AT+HTTPPARA="USERDATA","apikey: xxxxxxxxxxxxxxxxxxxxxx"
AT+HTTPPARA="USERDATA","apikey: xxxxxxxxxxxxxxxxxxxxxx"

OK
AT+HTTPPARA="USERDATA","Authorization: Bearer xxxxxxxxxxxxxxxxxxxxxx"
AT+HTTPPARA="USERDATA","Authorization: Bearer xxxxxxxxxxxxxxxxxxxxxx"

OK
AT+HTTPPARA="USERDATA","Prefer: return=representation"
AT+HTTPPARA="USERDATA","Prefer: return=representation"

OK
AT+HTTPPARA="CONTENT","application/json"
AT+HTTPPARA="CONTENT","application/json"

OK
AT+HTTPPARA="USERDATA","User-Agent: TinyGSM/LilyGo-Sim-Series"
AT+HTTPPARA="USERDATA","User-Agent: TinyGSM/LilyGo-Sim-Series"

OK
Posting HTTPS data to https://xxxxxxxxxx.supabase.co/rest/v1/locations ...
AT+HTTPDATA=71,10000
AT+HTTPDATA=71,10000

DOWNLOAD{
	"coordinates":	{
		"latitude":	39.9042,
		"longitude":	116.4074
	}
}

OK
AT+HTTPACTION=1
AT+HTTPACTION=1

OK

PB DONE

+HTTPACTION: 1,201,169
HTTP Response code: 201
AT+HTTPHEAD

AT+HTTPHEAD

+HTTPHEAD: 946
HTTP/1.1 201 Created
Date: Mon, 17 Nov 2025 05:49:57 GMT
Content-Type: application/json; charset=utf-8
Transfer-Encoding: chunked
Connection: keep-alive
Server: cloudflare
Content-Range: */*
CF-Ray: xxxxxxx-SIN
CF-Cache-Status: DYNAMIC
Strict-Transport-Security: max-age=31536000; includeSubDomains; preload
Vary: Accept-Encoding
content-profile: public
preference-applied: return=representation
sb-gateway-version: 1
sb-project-ref: xxxxxxxxxxx
sb-request-id: 019a905c-e01a-7722-ba2d-084f095e6d73
X-Content-Type-Options: nosniff
x-envoy-attempt-count: 1
x-envoy-upstream-service-time: 285
Set-Cookie: __cf_bm=5BaJfhkMxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx path=/; expires=Mon, 17-Nov-25 06:19:57 GMT; domain=.supabase.co; HttpOnly; Secure; SameSite=None
alt-svc: h3=":443"; ma=86400
OK
HTTP Header : HTTP/1.1 201 Created
Date: Mon, 17 Nov 2025 05:49:57 GMT
Content-Type: application/json; charset=utf-8
Transfer-Encoding: chunked
Connection: keep-alive
Server: cloudflare
Content-Range: */*
CF-Ray: xxxxxxxxxx-SIN
CF-Cache-Status: DYNAMIC
Strict-Transport-Security: max-age=31536000; includeSubDomains; preload
Vary: Accept-Encoding
content-profile: public
preference-applied: return=representation
sb-gateway-version: 1
sb-project-ref: xxxxxxxxxxxx
sb-request-id: 019a905c-xxxxxxx7722-ba2d-084f095e6d73
X-Content-Type-Options: nosniff
x-envoy-attempt-count: 1
x-envoy-upstream-service-time: 285
Set-Cookie: __cf_bm=5BaJfhkMUKsWN_a7Bkxpdq_Vxxxxxxxxxx.xxxxxxxx; path=/; expires=Mon, 17-Nov-25 06:19:57 GMT; domain=.supabase.co; HttpOnly; Secure; SameSite=None
alt-svc: h3=":443"; ma=86400
AT+HTTPREAD?
AT+HTTPREAD?

+HTTPREAD: LEN,169

OK
AT+HTTPREAD=0,169
AT+HTTPREAD=0,169

OK

+HTTPREAD: 169
[{"id":"xxxxx-8398-4bd5-b96e-ddcfc6d3e3f5","user_id":null,"coordinates":{"latitude": 39.9042, "longitude": 116.4074},"created_at":"2025-11-17T05:49:57.331346+00:00"}]
+HTTPREAD: 0HTTP body : [{"id":"xxxxx-8398-4bd5-b96e-ddcfc6d3e3f5","user_id":null,"coordinates":{"latitude": 39.9042, "longitude": 116.4074},"created_at":"2025-11-17T05:49:57.331346+00:00"}]
AT+HTTPTERM

AT+HTTPTERM



```

