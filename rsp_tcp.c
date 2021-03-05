/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012-2013 by Hoernchen <la@tfc-server.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <mirsdrapi-rsp.h>

#define closesocket close
#define SOCKADDR struct sockaddr
#define SOCKET int
#define SOCKET_ERROR -1

static SOCKET s;

static pthread_t tcp_worker_thread;
static pthread_t command_thread;
//static pthread_cond_t exit_cond;
//static pthread_mutex_t exit_cond_lock;

static pthread_mutex_t ll_mutex;
static pthread_cond_t cond;

struct llist {
	char *data;
	size_t len;
	struct llist *next;
};

typedef struct { /* structure size must be multiple of 2 bytes */
	char magic[4];
	uint32_t tuner_type;
	uint32_t tuner_gain_count;
} dongle_info_t;

double atofs(char *s)
/* standard suffixes */
{
	char last;
	int len;
	double suff = 1.0;
	len = strlen(s);
	last = s[len-1];
	s[len-1] = '\0';
	switch (last) {
		case 'g':
		case 'G':
			suff *= 1e3;
			/* fall-through */
		case 'm':
		case 'M':
			suff *= 1e3;
			/* fall-through */
		case 'k':
		case 'K':
			suff *= 1e3;
			suff *= atof(s);
			s[len-1] = last;
			return suff;
	}
	s[len-1] = last;
	return atof(s);
}

static int global_numq = 0;
static struct llist *ll_buffers = 0;
static int llbuf_num = 512;
static int ignore_f_command = 0;
static int ignore_s_command = 0;

static volatile int do_exit = 0;
static volatile int ctrlC_exit = 0;

#define MAX_DEVS 8
#define WORKER_TIMEOUT_SEC 3
#define DEFAULT_BW_T mir_sdr_BW_1_536
#define DEFAULT_AGC_SETPOINT -40 // original -24 //Bas -34
#define DEFAULT_GAIN_REDUCTION 44 // original 40 //Bas 34
#define DEFAULT_LNA 0 // 0 = off to 9 - Attenuator!
#define RTLSDR_TUNER_R820T 5
#define MAX_DECIMATION_FACTOR 32

static int devModel = 0;
static int bwType = DEFAULT_BW_T;
static int agcSetPoint = DEFAULT_AGC_SETPOINT;
static int gainReduction = DEFAULT_GAIN_REDUCTION;
static int rspLNA = DEFAULT_LNA;
static int infoOverallGr;
static int samples_per_packet;
static int last_gain_idx = 0;
static int verbose = 0;
static int wideband = 2; // wideband 0=small / 1=wide / 2=optimised for samplerate
static int edgefilter = 0;

////waardes
static int devAvail = 0;
static int device = 0;
static int antenna = 0;
static int enable_biastee = 0;
static int enable_dabnotch = 1;
static int enable_broadcastnotch = 1;
static int enable_refout = 0;
static int deci = 1;

////AGC beware to change all!
static int agc_type = mir_sdr_AGC_100HZ; //AGC 5-50-100HZ or DISABLE
static int agctype = 100; // just the number of above

static void sighandler(int signum)
{
	fprintf(stderr, "Signal (%d) caught, ask for exit!\n", signum);
	// exit(signum); // can be used to force ctrl-c instant
	do_exit = 1;
	pthread_cond_signal(&cond);
}

void gc_callback(unsigned int gRdB, unsigned int lnaGRdB, void* cbContext )
{
	if (gRdB == mir_sdr_ADC_OVERLOAD_DETECTED)
	{
		printf("adc overload detected\n");
		mir_sdr_GainChangeCallbackMessageReceived(); 
	}
	else if (gRdB == mir_sdr_ADC_OVERLOAD_CORRECTED)
	{
		printf("adc overload corrected\n");
		mir_sdr_GainChangeCallbackMessageReceived(); 
	}
	if (verbose)
		printf("new gain reduction (%d), lna gain reduction (%d)\n", gRdB, lnaGRdB);
}

void rx_callback(short *xi, short *xq, unsigned int firstSampleNum, int grChanged, int rfChanged, int fsChanged, unsigned int numSamples, unsigned int reset, unsigned int hwRemoved, void* cbContext)
{
        unsigned int i;
	short xi2=0;
	short xq2=0;
        if(!do_exit) {
                struct llist *rpt = (struct llist*)malloc(sizeof(struct llist));
		rpt->data = (char*)malloc(2 * numSamples);
			// assemble the data
                        unsigned char *data;
                        data = (unsigned char*)rpt->data;

			for (i = 0; i < numSamples; i++, xi++, xq++) {
				if (*xi < -1536 ) {
                                        xi2 = -1536;
                                }
                                else if (*xi > 1535 ) {
                                        xi2 = 1535;
                                }

                                if (*xq < -1536 ) {
                                        xq2 = -1536;
                                }
                                else if (*xq > 1535 ) {
                                        xq2 = 1535;
                                }

                                xi2 = *xi + 1536;
                                xq2 = *xq + 1536;

				*(data++) = (unsigned char)(xi2 / 12);
                                *(data++) = (unsigned char)(xq2 / 12);

// I/Q value reader - if enabled show values
//if (*xi > 1500 || *xi < -1500 || *xq > 1500 || *xq < -1500) {
//printf("xi=%hd,xq=%hd\n",(*xi),(*xq));}

                        rpt->len = 2 * numSamples;
                }

		rpt->next = NULL;

		pthread_mutex_lock(&ll_mutex);

		if (ll_buffers == NULL) {
			ll_buffers = rpt;
		} else {
			struct llist *cur = ll_buffers;
			int num_queued = 0;

			while (cur->next != NULL) {
				cur = cur->next;
				num_queued++;
			}

			if(llbuf_num && llbuf_num == num_queued-2){
				struct llist *curelem;

				free(ll_buffers->data);
				curelem = ll_buffers->next;
				free(ll_buffers);
				ll_buffers = curelem;
			}

			cur->next = rpt;

			global_numq = num_queued;
		}
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&ll_mutex);
	}
}

static void *tcp_worker(void *arg)
{
	struct llist *curelem,*prev;
	int bytesleft,bytessent, index;
	struct timeval tv= {1,0};
	struct timespec ts;
	struct timeval tp;
	fd_set writefds;
	int r = 0;

	while(1) {
		if(do_exit)
			pthread_exit(0);

		pthread_mutex_lock(&ll_mutex);
		gettimeofday(&tp, NULL);
		ts.tv_sec  = tp.tv_sec + WORKER_TIMEOUT_SEC;
		ts.tv_nsec = tp.tv_usec * 1000;
		r = pthread_cond_timedwait(&cond, &ll_mutex, &ts);
		if(r == ETIMEDOUT) {
			pthread_mutex_unlock(&ll_mutex);
			printf("worker cond timeout\n");
			sighandler(0);
			pthread_exit(NULL);
		}

		curelem = ll_buffers;
		ll_buffers = 0;
		pthread_mutex_unlock(&ll_mutex);

		while(curelem != 0) {
			bytesleft = curelem->len;
			index = 0;
			bytessent = 0;
			while(bytesleft > 0) {
				FD_ZERO(&writefds);
				FD_SET(s, &writefds);
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				r = select(s+1, NULL, &writefds, NULL, &tv);
				if(r) {
					bytessent = send(s,  &curelem->data[index], bytesleft, 0);
					bytesleft -= bytessent;
					index += bytessent;
				}
				if(bytessent == SOCKET_ERROR || do_exit) {
						printf("worker socket bye\n");
						sighandler(0);
						pthread_exit(NULL);
				}
			}
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}
	}
}

// gain reduction list in back order to emulate R820T gain
//--bas--
// const int gain_list[] = { 78, 75, 72, 69, 66, 63, 60, 57, 54, 51, 48, 45, 42, 39, 36, 33, 30, 27, 24, 21, 18, 15, 12, 9, 6, 3, 0 };
const int gain_list[] = { 40 };

static int set_gain_by_index(unsigned int index)
{
	int r;

//	gainReduction = gain_list[index];
//        r = mir_sdr_Reinit(&gainReduction, 0, 0, 0, 0, 0, rspLNA, &infoOverallGr, mir_sdr_USE_SET_GR_ALT_MODE, &samples_per_packet, mir_sdr_CHANGE_GR);
	r = mir_sdr_Reinit(&gainReduction, 0, 0, 0, 0, 0, rspLNA, &infoOverallGr, mir_sdr_USE_RSP_SET_GR, &samples_per_packet, mir_sdr_CHANGE_GR);
	if (r != mir_sdr_Success) {
		printf("set gain reduction error (%d)\n", r);
	}
	last_gain_idx = index;
	return r;
}
//end gain reduction

static int set_tuner_gain_mode(unsigned int mode)
{
	int r;

	if (mode)
	{
		r = mir_sdr_AgcControl(agc_type, agcSetPoint, 0, 0, 0, 0, rspLNA);
		r = set_gain_by_index(last_gain_idx);
		printf("agc disabled\n");
	}
	else
	{
//r = mir_sdr_AgcControl(mir_sdr_AGC_100HZ, agcSetPoint, 0, 0, 0, 0, rspLNA);
// Changes by PA0SIM =======================================
		r = mir_sdr_AgcControl(agc_type, agcSetPoint, 0, 0, 0, 0, rspLNA);
		printf("agc enabled\n");
	}
	if (r != mir_sdr_Success) {
		printf("tuner gain (agc) control error (%d)\n", r);
	}
	return r;
}

static int set_agc_mode(unsigned int mode)
{
	int r;

	if (mode)
	{
		//rspLNA = 1;
		printf("enable LNA\n");
	}
	else
	{
		//rspLNA = 0;
		printf("disable LNA\n");
	}
	r = set_gain_by_index(last_gain_idx);
	return r;
}

static int set_freq_correction(int32_t corr)
{
	int r;

	r = mir_sdr_SetPpm((double)corr);
	if (r != mir_sdr_Success) {
		printf("set freq correction error (%d)\n", r);
	}
	return r;
}

static int set_freq(uint32_t f)
{
	int r;

	r = mir_sdr_Reinit(&gainReduction, 0, (double)f/1e6, 0, bwType, 0, 0, &infoOverallGr, 0, &samples_per_packet, mir_sdr_CHANGE_RF_FREQ);
	if (r != mir_sdr_Success) {
		printf("set freq error (%d)\n", r);
	}
	return r;
}

static int set_sample_rate(uint32_t sr)
{
	int r;
	double f;

	if (sr < (2000000 / MAX_DECIMATION_FACTOR) || sr > 10000000) {
		fprintf(stderr, "sample rate %u is not supported\n", sr);
		return -1;
	}

	if (sr < 2000000)
	{
		int c = 0;

		// Find best decimation factor
		while (sr * (1 << c) < 2000000 && (1 << c) < MAX_DECIMATION_FACTOR) {
			c++;
		}

		deci = 1 << c;

		if (sr >= 1536000 && sr < 2000000)
		{
			if (wideband == 1) bwType = mir_sdr_BW_5_000;
			else bwType = mir_sdr_BW_1_536;
		}
		else
		if (sr >= 600000 && sr < 1536000)
		{
			if (wideband >= 1) bwType = mir_sdr_BW_1_536;
			else bwType = mir_sdr_BW_0_600;
		}
		else
		if (sr >= 300000 && sr < 600000)
		{
			if (wideband >= 1) bwType = mir_sdr_BW_0_600;
			else bwType = mir_sdr_BW_0_300;
		}
		else
		if (sr >= 200000 && sr < 300000)
                {
                        if (wideband >= 1) bwType = mir_sdr_BW_0_300;
                        else bwType = mir_sdr_BW_0_200;
                }
		else
		{
			bwType = mir_sdr_BW_0_200;
		}
	}
	else
	{
		deci =1;
		if (sr >= 8000000 && sr <= 10000000)
		{
			bwType = mir_sdr_BW_8_000;
		}
		else
		if (sr >= 7000000 && sr < 8000000)
		{
			bwType = mir_sdr_BW_7_000;
		}
		else
		if (sr >= 6000000 && sr < 7000000)
		{
			bwType = mir_sdr_BW_6_000;
		}
		else
		if (sr >= 5000000 && sr < 6000000)
		{
			bwType = mir_sdr_BW_5_000;
		}
		else
		if (sr >= 2500000 && sr < 5000000)
		{
			if (wideband >= 1 && sr >= 2880000) bwType = mir_sdr_BW_5_000;
			else bwType = mir_sdr_BW_1_536;
		}
		else
		{
			if (wideband == 1 && sr >= 2048000) bwType = mir_sdr_BW_5_000;
                        else bwType = mir_sdr_BW_1_536;

		}
	}

	f = (double)(sr * deci);

	if (deci <= 1 && edgefilter == 0)
		mir_sdr_DecimateControl(0, 0, 0);
	else if (deci >= 1 && edgefilter == 0)
                mir_sdr_DecimateControl(1, deci, 0);
	else if (deci >= 1 && edgefilter >= 1)
		mir_sdr_DecimateControl(1, deci, 1);

	printf("device SR %.2f, decim %d, output SR %u, IF Filter BW %d kHz\n", f, deci, sr, bwType);

//r = mir_sdr_Reinit(&gainReduction, (double)f/1e6, 0, bwType, 0, 0, 0, &infoOverallGr, 0, &samples_per_packet, mir_sdr_CHANGE_FS_FREQ | mir_sdr_CHANGE_BW_TYPE);
// Changes by PA0SIM ===========================
	r = mir_sdr_Reinit(&gainReduction, (double)f/1e6, 0, bwType, 0, 0, 0, &infoOverallGr, 0, &samples_per_packet, mir_sdr_CHANGE_FS_FREQ | mir_sdr_CHANGE_BW_TYPE | mir_sdr_CHANGE_IF_TYPE);
	if (r != mir_sdr_Success) {
		printf("set sample rate error (%d)\n", r);
	}
	return r;
}

struct command{
	unsigned char cmd;
	unsigned int param;
}__attribute__((packed));

static void *command_worker(void *arg)
{
	int left, received = 0;
	fd_set readfds;
	struct command cmd={0, 0};
	struct timeval tv= {1, 0};
	int r = 0;
	uint32_t tmp;

	while(1) {
		left=sizeof(cmd);
		while(left >0) {
			FD_ZERO(&readfds);
			FD_SET(s, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(s+1, &readfds, NULL, NULL, &tv);
			if(r) {
				received = recv(s, (char*)&cmd+(sizeof(cmd)-left), left, 0);
				left -= received;
			}
			if(received == SOCKET_ERROR || do_exit) {
				printf("comm recv bye\n");
				sighandler(0);
				pthread_exit(NULL);
			}
		}
		switch(cmd.cmd) {
		case 0x01:
//			printf("set freq %d\n", ntohl(cmd.param));
//			set_freq(ntohl(cmd.param));
			if (ignore_f_command) {
				printf("set freq %d ignored because -f used at commandline\n", ntohl(cmd.param));
			} else {
				printf("set freq %d\n", ntohl(cmd.param));
				set_freq(ntohl(cmd.param));
			}
			break;
		case 0x02:
//                        printf("set sample rate %d\n", ntohl(cmd.param));
//                        set_sample_rate(ntohl(cmd.param));
                        if (ignore_s_command) {
                                printf("set sample rate %d ignored because -s used at commandline\n", ntohl(cmd.param));
                        } else {
                                printf("set sample rate %d\n", ntohl(cmd.param));
                                set_sample_rate(ntohl(cmd.param));
                        }
                        break;
		case 0x03:
			printf("set gain mode %d\n", ntohl(cmd.param));
			set_tuner_gain_mode(ntohl(cmd.param));
			break;
		case 0x04:
			printf("set gain %d\n", ntohl(cmd.param));
			// rtlsdr_set_tuner_gain(dev, ntohl(cmd.param));
			break;
		case 0x05:
			printf("set freq correction %d\n", ntohl(cmd.param));
			set_freq_correction(ntohl(cmd.param));
			break;
		case 0x06:
			tmp = ntohl(cmd.param);
			printf("set if stage %d gain %d\n", tmp >> 16, (short)(tmp & 0xffff));
			// rtlsdr_set_tuner_if_gain(dev, tmp >> 16, (short)(tmp & 0xffff));
			break;
		case 0x07:
			printf("set test mode %d\n", ntohl(cmd.param));
			// rtlsdr_set_testmode(dev, ntohl(cmd.param));
			break;
		case 0x08:
			printf("set agc mode %d\n", ntohl(cmd.param));
			set_agc_mode(ntohl(cmd.param));
			break;
		case 0x09:
			printf("set direct sampling %d\n", ntohl(cmd.param));
			// rtlsdr_set_direct_sampling(dev, ntohl(cmd.param));
			break;
		case 0x0a:
			printf("set offset tuning %d\n", ntohl(cmd.param));
			// rtlsdr_set_offset_tuning(dev, ntohl(cmd.param));
			break;
		case 0x0b:
			printf("set rtl xtal %d\n", ntohl(cmd.param));
			// rtlsdr_set_xtal_freq(dev, ntohl(cmd.param), 0);
			break;
		case 0x0c:
			printf("set tuner xtal %d\n", ntohl(cmd.param));
			// rtlsdr_set_xtal_freq(dev, 0, ntohl(cmd.param));
			break;
		case 0x0d:
			printf("set tuner gain by index %d\n", ntohl(cmd.param));
			set_gain_by_index(ntohl(cmd.param));
			break;
		case 0x0e:
			printf("set bias tee %d\n", ntohl(cmd.param));
			// rtlsdr_set_bias_tee(dev, (int)ntohl(cmd.param));
			break;
		default:
			break;
		}
		cmd.cmd = 0xff;
		fflush(stdout);
	}
}

void usage(void)
{
	printf("rsp_tcp, an I/Q spectrum server for SDRPlay receivers - modified by Bas ON5HB for websdr.org "
#ifdef SERVER_VERSION
		"VERSION "SERVER_VERSION
#endif
		"\n\n Usage:\n"
		"\t-a Listen address (default: 127.0.0.1)\n"
		"\t-p Listen port (default: 1234)\n"
		"\t-d RSP device to use (default: 1, first found)\n"
		"\t-P Antenna Port select (0/1/2, default: 0, Port A)\n"
		"\t-r Gain reduction (default: 44  / values 20-59) - Not set in websdr.cfg see tips\n"
		"\t-l LNA attenuator level about -6dB each step (default: 0 / values 0-9) - See tips below\n"
		"\t-T Bias-T enable* (default: disabled)\n"
		"\t-f Frequency to tune center in Hertz (default: 1000000 = 1MHz) - If freq set centerfreq and progfreq are ignored! - Normally set in websdr.cfg\n"
		"\t-s Samplerate in samples-per-second (default: 2048000) - If sample rate is set it will be ignored from client! - Normally set in websdr.cfg\n\n"
		"\t-W Wideband-pass-filter mode (default: 2 / values: 0 small / 1 wide / 2 = optimised)\n"
		"\t-D DAB band-reject-filter* (default: enabled)\n"
                "\t-B MW band-reject-filter* (default: enabled)\n"
                "\t-R Refclk output* (default: disabled)\n"
		"\t-E Edge-steep-filter enable* (default: disabled) - Beware CPU load could go high!\n\n"
		"\t-A Auto Gain Control setpoint (default: -40 / values -1 to -69 / other disabled)\n"
		"\t-G Auto Gain Control speed in Hz (default: 100 / values 0/5/50/100) - Sets overloading adjustment-speed\n"
		"\t-n Max number of linked list buffers to keep (default: 512)\n"
		"\t-v Verbose output (debug) enable* (default: disabled)\n\n"
		"\t* Marked options are switches they toggle on/off\n"
		"\t Tip1 - start with -l 0 level correction first, if too much signal try -l 1, -l 2 etc, most simple way start receiving correctly\n"
		"\t Tip2 - websdr.cfg gain setting is cosmetic only, it does not impact the receivers performance!\n"
		"\t Tip3 - if you have ghost-signals, set wideband-pass to small as it's probably too wide, mostly happens on HF!\n\n"
		 );
	exit(1);
}

int main(int argc, char **argv)
{
	int r, opt, i;
	char* addr = "127.0.0.1";
	int port = 1234;
	uint32_t frequency = 1000000;
	uint32_t samp_rate = 2048000;
	struct sockaddr_in local, remote;
	struct llist *curelem,*prev;
	pthread_attr_t attr;
	void *status;
	struct timeval tv = {1,0};
	struct linger ling = {1,0};
	SOCKET listensocket;
	socklen_t rlen;
	fd_set readfds;
	dongle_info_t dongle_info;

	float ver;
	mir_sdr_DeviceT devices[MAX_DEVS];
	unsigned int numDevs;
/////waardes

	struct sigaction sigact, sigign;

	while ((opt = getopt(argc, argv, "a:p:r:f:s:n:d:l:P:A:G:W:TvDBRE")) != -1) {
		switch (opt) {
		case 'd':
			device = atoi(optarg) - 1;
			break;
		case 'P':
			antenna = atoi(optarg);
			break;
		case 'r':
			gainReduction = atoi(optarg);
			break;
		case 'f':
			frequency = (uint32_t)atofs(optarg);
			ignore_f_command = 1;
			break;
		case 's':
			samp_rate = (uint32_t)atofs(optarg);
			ignore_s_command = 1;
			break;
		case 'A':
                        agcSetPoint = atoi(optarg);
                        break;
		case 'a':
			addr = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'n':
			llbuf_num = atoi(optarg);
			break;
                case 'W':
                        wideband = atoi(optarg);
                        break;
		case 'E':
                        edgefilter = 1;
                        break;
		case 'l':
			rspLNA = atoi(optarg);
			break;
                case 'G':
                        agctype = atoi(optarg);
                        break;
		case 'T':
			enable_biastee = 1;
			break;
		case 'D':
                        enable_dabnotch = 0;
                        break;
		case 'B':
			enable_broadcastnotch = 0;
			break;
		case 'R':
			enable_refout = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
			break;
		}
	}

	if (agctype == 5) agc_type = mir_sdr_AGC_5HZ;
	else if (agctype == 50) agc_type = mir_sdr_AGC_50HZ;
	else if (agctype == 100) agc_type = mir_sdr_AGC_100HZ;
	else { agc_type = mir_sdr_AGC_DISABLE;
		agctype = 0;}

	if (gainReduction < 20 || gainReduction > 59) gainReduction = DEFAULT_GAIN_REDUCTION;
	if (wideband < 0 || wideband > 2 ) wideband = 2;

	printf("\nrsp_tcp, an I/Q spectrum server for SDRPlay receivers - modified by Bas ON5HB for websdr.org "
        	#ifdef SERVER_VERSION
                        "VERSION "SERVER_VERSION
                #endif
                );
                printf("\nuse --help for options and explenation\n\n");

	// check API version
	r = mir_sdr_ApiVersion(&ver);
	if (ver != MIR_SDR_API_VERSION) {
		//  Error detected, include file does not match dll. Deal with error condition.
		printf("library libmirsdrapi-rsp must be version %f\n", ver);
		exit(1);
	}
	printf("libmirsdrapi-rsp version %.2f found\n", ver);

	// enable debug output
	if (verbose)
		mir_sdr_DebugEnable(1);

        // select RSP device
        r = mir_sdr_GetDevices(&devices[0], &numDevs, MAX_DEVS);
        if (r != mir_sdr_Success) {
                fprintf(stderr, "Failed to get device list (%d)\n", r);
                exit(1);
        }

        for (i = 0; i < numDevs; i++) {
                if (devices[i].devAvail == 1) {
                        devAvail++;
                }
        }

        if (devAvail == 0) {
                fprintf(stderr, "no RSP devices available.\n");
                exit(1);
        }

        if (devices[device].devAvail != 1) {
                fprintf(stderr, "RSP selected (%d) is not available.\n", (device + 1));
                exit(1);
        }

        r = mir_sdr_SetDeviceIdx(device);
        if (r != mir_sdr_Success) {
                fprintf(stderr, "Failed to set device index (%d)\n", r);
                exit(1);
        }

	// get RSP model and display modelname.
	devModel = devices[device].hwVer;
	if (devModel == 1) printf("detected RSP model (hw version %d) = RSP1\n", devModel);
	else if (devModel == 2) printf("detected RSP model (hw version %d) = RSP2\n", devModel);
	else if (devModel == 3) printf("detected RSP model (hw version %d) = RSPduo\n", devModel);
	else if (devModel == 255) printf("detected RSP model (hw version %d) = RSP1A\n", devModel);
	else printf("detected RSP model (hw version %d) = Unknown\n", devModel);

	// select antenna
	switch (antenna) {
		case 1:
			mir_sdr_RSPII_AntennaControl(mir_sdr_RSPII_ANTENNA_B);
			mir_sdr_AmPortSelect(0);
			break;
		case 2:
			mir_sdr_AmPortSelect(1);
			break;
		default:
			mir_sdr_RSPII_AntennaControl(mir_sdr_RSPII_ANTENNA_A);
			mir_sdr_AmPortSelect(0);
	}

	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);

	//pthread_mutex_init(&exit_cond_lock, NULL);
	pthread_mutex_init(&ll_mutex, NULL);
	//pthread_mutex_init(&exit_cond_lock, NULL);
	pthread_cond_init(&cond, NULL);
	//pthread_cond_init(&exit_cond, NULL);

	memset(&local,0,sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = inet_addr(addr);

	listensocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	r = 1;
	setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, (char *)&r, sizeof(int));
	setsockopt(listensocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));
	bind(listensocket,(struct sockaddr *)&local,sizeof(local));

	r = fcntl(listensocket, F_GETFL, 0);
	r = fcntl(listensocket, F_SETFL, r | O_NONBLOCK);

	while(1) {
		printf("listening...\n");
		printf("Use the device argument 'rsp_tcp=%s:%d' in OsmoSDR "
		       "(gr-osmosdr) source\n"
		       "to receive samples in GRC and control "
		       "rsp_tcp parameters (frequency, gain, ...).\n",
		       addr, port);
		listen(listensocket,1);

		while(1) {
			FD_ZERO(&readfds);
			FD_SET(listensocket, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(listensocket+1, &readfds, NULL, NULL, &tv);
			if(do_exit) {
				goto out;
			} else if(r) {
				rlen = sizeof(remote);
				s = accept(listensocket,(struct sockaddr *)&remote, &rlen);
				break;
			}
		}

		setsockopt(s, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

		printf("client accepted!\n");
                printf("AGC-type set %dHz (0 means disabled)\n", agctype);
                printf("Low-Noise-Amp mode set %u \n", rspLNA);
                printf("Gain-Reduction set %d \n", gainReduction);
                printf("AGC-Gain-Setpoint set %d \n", agcSetPoint);
                printf("Edgefilter set %d (0=off 1=on)\n", edgefilter);

		memset(&dongle_info, 0, sizeof(dongle_info));
		memcpy(&dongle_info.magic, "RTL0", 4);

		dongle_info.tuner_type = htonl(RTLSDR_TUNER_R820T);
		dongle_info.tuner_gain_count = htonl(sizeof(gain_list)/sizeof(gain_list[0]) - 1);

		r = send(s, (const char *)&dongle_info, sizeof(dongle_info), 0);
		if (sizeof(dongle_info) != r)
			printf("failed to send dongle information\n");

		// must start the tcp_worker before the first samples are available from the rx
		// because the rx_callback tries to send a condition to the worker thread
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
		r = pthread_create(&tcp_worker_thread, &attr, tcp_worker, NULL);
/*
		r = pthread_create(&command_thread, &attr, command_worker, NULL);
		pthread_attr_destroy(&attr);
*/
		// initialise API and start the rx
		// r = mir_sdr_StreamInit(&gainReduction, (samp_rate/1e6), (frequency/1e6), bwType, 0, rspLNA, &infoOverallGr, mir_sdr_USE_SET_GR_ALT_MODE, &samples_per_packet, rx_callback, gc_callback, (void *)NULL);
		// Changes by PA0SIM =============================
		r = mir_sdr_StreamInit(&gainReduction, (samp_rate/1e6), (frequency/1e6), bwType, 0, rspLNA, &infoOverallGr, mir_sdr_USE_RSP_SET_GR, &samples_per_packet, rx_callback, gc_callback, (void *)NULL);
		if (r != mir_sdr_Success)
		{
			printf("failed to start the RSP device, return (%d)\n", r);
			break;
		}
		fprintf(stderr,"started rx\n");

		//Notches and other stuff must be here!!....

		// enable DC offset and IQ imbalance correction
	        mir_sdr_DCoffsetIQimbalanceControl(1, 1);
	        // enable AGC with a setPoint of -30dBfs
	        mir_sdr_AgcControl(agc_type, agcSetPoint, 0, 0, 0, 0, rspLNA);
	        // set the DC offset correction mode for the tuner (moved from below)
	        mir_sdr_SetDcMode(4, 1);
	        // set the time period over which the DC offset is tracked when in one shot mode.
	        mir_sdr_SetDcTrackTime(10);
	        // set Bias-T
	        mir_sdr_RSPII_BiasTControl(enable_biastee);
	        mir_sdr_rsp1a_BiasT(enable_biastee);
	        mir_sdr_rspDuo_BiasT(enable_biastee);
	        // set Notch
	        mir_sdr_RSPII_RfNotchEnable(enable_broadcastnotch);
	        mir_sdr_rsp1a_DabNotch(enable_dabnotch);
	        mir_sdr_rsp1a_BroadcastNotch(enable_broadcastnotch);
	        mir_sdr_rspDuo_DabNotch(enable_dabnotch);
	        mir_sdr_rspDuo_BroadcastNotch(enable_broadcastnotch);
	        mir_sdr_rspDuo_Tuner1AmNotch(enable_broadcastnotch);
	        // set external reference output
	        mir_sdr_RSPII_ExternalReferenceControl(enable_refout);
        	mir_sdr_rspDuo_ExtRef(enable_refout);

		// the rx must be started before accepting commands from the command worker
		r = pthread_create(&command_thread, &attr, command_worker, NULL);
		pthread_attr_destroy(&attr);

		// wait for the workers to exit
		pthread_join(tcp_worker_thread, &status);
		pthread_join(command_thread, &status);

		closesocket(s);
//
		// stop the receiver
		mir_sdr_StreamUninit();
//
		printf("all threads dead..\n");

		curelem = ll_buffers;
		ll_buffers = 0;

		while(curelem != 0) {
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}

		global_numq = 0;

		do_exit = 0;
	}

out:
	mir_sdr_StreamUninit();
	mir_sdr_ReleaseDeviceIdx();

	closesocket(listensocket);
	closesocket(s);
	printf("bye!\n");
	return r >= 0 ? r : -r;
}
