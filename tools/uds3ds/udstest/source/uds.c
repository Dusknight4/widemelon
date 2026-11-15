// udstest: a 3DS local wireless (UDS) self-test, for MultiMelon's "Wifi as Local Multiplayer" (in the
// emulator, and on a real 3DS with the UDS bridge module). It scans for a "udstest" network for about
// three seconds, joins it if it finds one, else hosts it; then for 20 seconds it sends a numbered packet
// every 10 frames and counts the ones it receives. Everything goes to the screen and to sdmc:/udstest.txt,
// ending with "RESULT PASS" (it received packets from another console) or "RESULT FAIL".

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* g_log;

static void say(const char* fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vprintf(fmt, a);
	va_end(a);
	if (g_log)
	{
		va_start(a, fmt);
		vfprintf(g_log, fmt, a);
		va_end(a);
		fflush(g_log);
	}
}

#define MAGIC 0x55445354    // "UDST"

// fixed heaps (libctru otherwise takes all the free memory, which fails beside a Luma plugin in Azahar)
u32 __ctru_heap_size = 0x800000;
u32 __ctru_linear_heap_size = 0x400000;

int main(void)
{
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	g_log = fopen("sdmc:/udstest.txt", "w");
	say("udstest\n");

	Result ret = udsInit(0x3000, NULL);
	say("udsInit: %08lx\n", ret);
	if (R_SUCCEEDED(ret))
	{
		const u32 wlancommID = 0x4D4D3344;     // "MM3D"
		const char* pass = "multimelon udstest";
		const u8 channel = 1;
		udsBindContext bind;
		udsNetworkStruct net;
		udsNetworkScanInfo* networks = NULL;
		size_t total = 0;
		const size_t bufsize = 0x4000;
		u32* buf = malloc(bufsize);
		bool host = false;

		for (int i = 0; i < 3 && total == 0; i++)
		{
			ret = udsScanBeacons(buf, bufsize, &networks, &total, wlancommID, 0, NULL, false);
			say("scan %d: %08lx, %u found\n", i, ret, (unsigned)total);
			if (total == 0) svcSleepThread(1000000000LL);
		}
		if (total > 0)
		{
			ret = udsConnectNetwork(&networks[0].network, pass, strlen(pass) + 1, &bind, UDS_BROADCAST_NETWORKNODEID,
			                        UDSCONTYPE_Client, channel, UDS_DEFAULT_RECVBUFSIZE);
			say("join: %08lx\n", ret);
		}
		else
		{
			host = true;
			udsGenerateDefaultNetworkStruct(&net, wlancommID, 0, UDS_MAXNODES);
			ret = udsCreateNetwork(&net, pass, strlen(pass) + 1, &bind, channel, UDS_DEFAULT_RECVBUFSIZE);
			say("host: %08lx\n", ret);
		}
		free(networks);
		free(buf);

		if (R_SUCCEEDED(ret))
		{
			u32 sent = 0, received = 0, lastSeq = 0;
			u16 lastFrom = 0;
			int frame = 0;
			const u64 start = osGetTime();
			while (aptMainLoop() && osGetTime() - start < 20000)
			{
				gspWaitForVBlank();
				hidScanInput();
				if (++frame % 10 == 0)
				{
					u32 msg[2] = {MAGIC, sent};
					ret = udsSendTo(UDS_BROADCAST_NETWORKNODEID, channel, UDS_SENDFLAG_Default, msg, sizeof(msg));
					if (UDS_CHECK_SENDTO_FATALERROR(ret))
					{
						say("send: %08lx\n", ret);
						break;
					}
					if (R_SUCCEEDED(ret)) sent++;
				}
				u32 rx[16];
				size_t got = 0;
				u16 from = 0;
				ret = udsPullPacket(&bind, rx, sizeof(rx), &got, &from);
				if (R_SUCCEEDED(ret) && got >= 8 && rx[0] == MAGIC)
				{
					received++;
					lastSeq = rx[1];
					lastFrom = from;
				}
			}
			udsConnectionStatus st;
			udsGetConnectionStatus(&st);
			say("%s: sent %lu, received %lu (last #%lu from node %u), nodes %u\n", host ? "host" : "client",
			    sent, received, lastSeq, lastFrom, st.total_nodes);
			say(received > 0 ? "RESULT PASS\n" : "RESULT FAIL\n");
			if (host) udsDestroyNetwork();
			else udsDisconnectNetwork();
			udsUnbind(&bind);
		}
		else
			say("RESULT FAIL\n");
		udsExit();
	}
	if (g_log) fclose(g_log);
	g_log = NULL;
	printf("Press START to exit.\n");
	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
	}
	gfxExit();
	return 0;
}
