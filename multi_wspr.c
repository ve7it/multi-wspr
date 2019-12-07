/*
 * FreeBSD License
 * Copyright (c) 2016, Guenael
 * All rights reserved.
 *
 * This file is based on AirSpy project & HackRF project
 *   Copyright 2012 Jared Boone <jared@sharebrained.com>
 *   Copyright 2014-2015 Benjamin Vernoux <bvernoux@airspy.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>

#include <libairspy/airspy.h>

#include "multi_wspr.h"
#include "wsprd.h"
#include "wsprnet.h"
#include "filter.h"
#include "freqsets.h"

extern int startairspy(void);
extern void stopairspy(void);

#define SIGNAL_LENGHT      116
#define SIGNAL_SAMPLE_RATE 375

/* Global declaration for these structs */
struct decoder_options  dec_options;

struct receiver_options rx_options;

/*  Input buffers  */
typedef struct sigbuff_s sigbuff_t;
struct sigbuff_s {
        sigbuff_t *next;
	uint32_t len;
    	float *iSamples;
    	float *qSamples;
};

typedef struct {
	bool decode_flag;
	uint32_t fr;

    	float *iosc,*qosc;
    	uint32_t ndi;
    	uint32_t mixerphase;

	sigbuff_t *sigbuff_head;
	sigbuff_t *sigbuff_tail;
	pthread_cond_t   sigbuff_cond;
	pthread_mutex_t  sigbuff_mutex;

	/* averaging */
    	double Ix,Qx;
    	uint32_t avridx;
    	/* FIR buffers */
	uint32_t hbp;
    	float   hbfirI[8][32], hbfirQ[8][32];
	uint32_t hbidx[8];
    	float   pfirI[FIRLEN], pfirQ[FIRLEN];
    	uint32_t polyphase;

    	/* WSPR decoder use buffers of 45000 samples (hardcoded)
       	(120 sec max @ 375sps = 45000 samples)
    	*/
    	float iSamples[45000];
    	float qSamples[45000];
    	uint32_t write_len;

    	pthread_t       wthread;
} channel_t;
channel_t channels[4];

/* local oscillators generator */
static void genosc(int s, int f , channel_t *chann)
{
  uint32_t i,ndi,fr;

  ndi=wsprfrsets[s].ndi[f];
  fr=AIRSPY_SAMPLE_RATE/4-(wsprfrsets[s].fr[f]-wsprfrsets[s].centerfr);

  chann->iosc=malloc(ndi*sizeof(float));
  chann->qosc=malloc(ndi*sizeof(float));

  for(i=0;i<ndi;i++) {
        double phase=2.0*M_PI*(double)i*(double)fr/AIRSPY_SAMPLE_RATE;
        chann->iosc[i]=cos(phase);
        chann->qosc[i]=sin(phase);
  }

 chann->ndi=ndi;
}

/* callback function called by airspy lib */
int rx_callback(airspy_transfer_t* transfer) {
    float *sigIn = (float *) transfer->samples;
    uint32_t sigLenght = transfer->sample_count;

    uint32_t n,len;
    
    for(n=0;n<wsprfrsets[rx_options.fset].nbfr;n++) {
	channel_t *chann=&(channels[n]);
        sigbuff_t *sbuff;

        if (chann->decode_flag == true) 
		continue; 

    	sbuff=malloc(sizeof(sigbuff_t));
    	sbuff->iSamples=malloc((sigLenght/125+1)*sizeof(float));
    	sbuff->qSamples=malloc((sigLenght/125+1)*sizeof(float));
    	sbuff->len = 0;
    	sbuff->next = NULL;

    	//printf("1:rx_callback %d %d\n",rx_state.iqIndex, rx_state.decode_flag);

        len=0;
        for(int32_t i=0; i<sigLenght; i++) {
	  	float s;

    	  	/* mixer + 125x downsampling by simple averaging  */
		s=sigIn[i];
   		chann->Ix += s*chann->iosc[chann->mixerphase];
  		chann->Qx += s*chann->qosc[chann->mixerphase];

		chann->mixerphase=(chann->mixerphase+1) % chann->ndi;

		chann->avridx++;
		if(chann->avridx<125)
			continue;

        	sbuff->iSamples[len] = chann->Ix;
        	sbuff->qSamples[len] = chann->Qx;
		len++;

		chann->avridx=0; chann->Ix=chann->Qx=0;
    	}
    	sbuff->len=len;
  
   	/* put in list at tail */
   	pthread_mutex_lock(&(chann->sigbuff_mutex));
   	if(chann->sigbuff_tail)
        	chann->sigbuff_tail->next=sbuff;
   	else
        	chann->sigbuff_head=sbuff;
   	chann->sigbuff_tail=sbuff;
   	pthread_cond_signal(&(chann->sigbuff_cond));
   	pthread_mutex_unlock(&(chann->sigbuff_mutex));
   }

   return 0;
}

/* fir x2 downsampler filters hard inlining */
#define firhalf(k) { \
 uint32_t idx=chann->hbidx[(k)]; \
 \
 chann->hbfirI[(k)][idx]=Ix; \
 chann->hbfirQ[(k)][idx]=Qx; \
 idx=(idx+1)%hbfsz[(k)]; \
 chann->hbidx[(k)]=idx; \
 if((chann->hbp>>(k))&1) continue; \
 \
 Ix=Qx=0; \
 for(j=0;j<hbfsz[(k)];j++) { \
      uint32_t s=hbfsz[(k)]-chann->hbidx[(k)]+j; \
      Ix+=chann->hbfirI[(k)][j]*hbfilter[(k)][s]; \
      Qx+=chann->hbfirQ[(k)][j]*hbfilter[(k)][s]; \
 } \
}
 
static void *wsprDecoder(void *arg) {
    channel_t *chann=arg;

    while (!dec_options.exit_flag) {
    	sigbuff_t *sbuff;

        pthread_mutex_lock(&(chann->sigbuff_mutex));
        while(chann->sigbuff_head == NULL && !dec_options.exit_flag)
		pthread_cond_wait(&(chann->sigbuff_cond), &(chann->sigbuff_mutex));

        if(dec_options.exit_flag) {
        	pthread_mutex_unlock(&(chann->sigbuff_mutex));
		pthread_exit(NULL);
	}

   	sbuff=chann->sigbuff_head;
        chann->sigbuff_head=chann->sigbuff_head->next;
        if(chann->sigbuff_head == NULL) chann->sigbuff_tail=NULL;
        pthread_mutex_unlock(&(chann->sigbuff_mutex));

        //printf("1:wsprdecode %d %d\n",sbuff->len,chann->write_len);

    	for(uint32_t i=0; i<sbuff->len; i++) {
		uint32_t j,k;
		double Ix,Qx;

       		Ix = sbuff->iSamples[i];
       		Qx = sbuff->qSamples[i];

		/* downsample x2  filters */
		chann->hbp++;
		firhalf(0);	
		firhalf(1);	
		firhalf(2);	
		firhalf(3);	
		firhalf(4);	
		firhalf(5);	
		firhalf(6);	
		firhalf(7);	

        	/* FIR  polyphase 5/3 downsampler */
        	for (j=0; j<FIRLEN-1; j++) {
                	chann->pfirI[j] = chann->pfirI[j+1];
                	chann->pfirQ[j] = chann->pfirQ[j+1];
		}
       		chann->pfirI[FIRLEN-1] = Ix;
       		chann->pfirQ[FIRLEN-1] = Qx;

		chann->polyphase+=3;
		if(chann->polyphase<5) continue ;
		chann->polyphase-=5;

        	Ix=Qx=0.0;
        	for (j=0, k=chann->polyphase; k<PFIRSZ; j++,k+=3) {
            		Ix += chann->pfirI[j]*zCoef[k];
            		Qx += chann->pfirQ[j]*zCoef[k];
        	}

        	/* Save the result in the buffer */
        	chann->iSamples[chann->write_len]=Ix;
        	chann->qSamples[chann->write_len]=Qx;
		chann->write_len++;
		if(chann->write_len >= 45000) break;
	 }

	free(sbuff->iSamples);
	free(sbuff->qSamples);
	free(sbuff);

        if (chann->write_len<SIGNAL_LENGHT*SIGNAL_SAMPLE_RATE) continue;
        chann->decode_flag = true ;

        //printf("2:wsprdecode %d \n",chann->write_len);

        /* Search & decode the signal */
        wspr_decode(chann->iSamples, chann->qSamples, chann->write_len, chann->fr);
	chann->write_len=0;

        //printf("3:wsprdecode done \n");
    }
    
    pthread_exit(NULL);
}


double atofs(char *s) {
    /* standard suffixes */
    char last;
    uint32_t len;
    double suff = 1.0;
    len = strlen(s);
    last = s[len-1];
    s[len-1] = '\0';
    switch (last) {
    case 'g':
    case 'G':
        suff *= 1e3;
    case 'm':
    case 'M':
        suff *= 1e3;
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


int32_t parse_u64(char* s, uint64_t* const value) {
    uint_fast8_t base = 10;
    char* s_end;
    uint64_t u64_value;

    if( strlen(s) > 2 ) {
        if( s[0] == '0' ) {
            if( (s[1] == 'x') || (s[1] == 'X') ) {
                base = 16;
                s += 2;
            } else if( (s[1] == 'b') || (s[1] == 'B') ) {
                base = 2;
                s += 2;
            }
        }
    }

    s_end = s;
    u64_value = strtoull(s, &s_end, base);
    if( (s != s_end) && (*s_end == 0) ) {
        *value = u64_value;
        return AIRSPY_SUCCESS;
    } else {
        return AIRSPY_ERROR_INVALID_PARAM;
    }
}


/* Reset flow control variable */
void initSampleStorage() {
    uint32_t n;
   
   for(n=0;n<wsprfrsets[rx_options.fset].nbfr;n++) { 
	channel_t *chann=&(channels[n]);
	int n;

   	/* empty sigbuff queue */
	n=0;
    	pthread_mutex_lock(&(chann->sigbuff_mutex));
    	while((chann->sigbuff_head)) {
		sigbuff_t *sbuff=chann->sigbuff_head;
        	chann->sigbuff_head=chann->sigbuff_head->next;
		free(sbuff->iSamples);
		free(sbuff->qSamples);
		free(sbuff);
		n++;
    	}
    	chann->sigbuff_tail=NULL;

    	chann->decode_flag = false;

    	pthread_mutex_unlock(&(chann->sigbuff_mutex));
 }
}

/* Default options for the decoder */
void initDecoder_options() {
    dec_options.usehashtable = 1;
    dec_options.npasses = 2;
    dec_options.subtraction = 1;
    dec_options.quickmode = 0;
}


/* Default options for the receiver */
void initrx_options() {
    rx_options.linearitygain= 12;
    rx_options.bias = 0;       // No bias
    rx_options.shift = 0;
    rx_options.serialnumber = 0;
    rx_options.packing = 0;
}


void sigint_callback_handler(int signum) {
    uint32_t n;

    fprintf(stdout, "Caught signal %d\n", signum);
    dec_options.exit_flag = true;
   
    for(n=0;n<wsprfrsets[rx_options.fset].nbfr;n++) { 
    	pthread_cond_broadcast(&(channels[n].sigbuff_cond));
    }
}


void usage(void) {
    fprintf(stderr,
            "airspy_wsprd, a simple WSPR daemon for AirSpy receivers\n\n"
            "Use:\tairspy_wsprd -f frequency -c callsign -g locator [options]\n"
            "\t-f frequency set.\n"
            "\t-c your callsign (12 chars max)\n"
            "\t-g your locator grid (6 chars max)\n"
            "Receiver extra options:\n"
            "\t-l linearity gain [0-21] (default: 12)\n"
            "\t-b set Bias Tee [0-1], (default: 0 disabled)\n"
            "\t-r sampling rate [2.5M, 3M, 6M, 10M], (default: 2.5M)\n"
            "\t-p frequency correction (default: 0)\n"
            "\t-u upconverter (default: 0, example: 125M)\n"
            "\t-s S/N: Open device with specified 64bits serial number\n"
            "\t-k packing: Set packing for samples, \n"
            "\t   1=enabled(12bits packed), 0=disabled(default 16bits not packed)\n"
            "Decoder extra options:\n"
            "\t-H do not use (or update) the hash table\n"
            "\t-Q quick mode, doesn't dig deep for weak signals\n"
            "\t-S single pass mode, no subtraction (same as original wsprd)\n"
            "Example:\n"
            "\tairspy_wsprd -f 144.489M -r 2.5M -c A1XYZ -g AB12cd -l 10 -m 7 -v 7\n");
    exit(1);
}


int main(int argc, char** argv) {
    uint32_t opt;
    uint32_t exit_code = EXIT_SUCCESS;
    uint32_t n;
    bool fst = true ; 

    initrx_options();
    initDecoder_options();

    /* Stop condition setup */
    dec_options.exit_flag   = false;

    if (argc <= 1)
        usage();

    while ((opt = getopt(argc, argv, "f:c:g:r:l:b:s:p:u:k:H:Q:S")) != -1) {
        switch (opt) {
        case 'c': // Callsign
            sprintf(dec_options.rcall, "%.12s", optarg);
            break;
        case 'g': // Locator / Grid
            sprintf(dec_options.rloc, "%.6s", optarg);
            break;
        case 'f': // freq set
            rx_options.fset = (uint32_t)atoi(optarg);
            break;
        case 'l': // LNA gain
            rx_options.linearitygain = (uint32_t)atoi(optarg);
            if (rx_options.linearitygain < 0) rx_options.linearitygain = 0;
            if (rx_options.linearitygain > 21 ) rx_options.linearitygain = 21;
            break;
        case 'b': // Bias setting
            rx_options.bias = (uint32_t)atoi(optarg);
            if (rx_options.bias <= 0) rx_options.bias = 0;
            if (rx_options.bias >= 1) rx_options.bias = 1;
            break;
        case 's': // Serial number
            parse_u64(optarg, &rx_options.serialnumber);
            break;
        case 'p': // Fine frequency correction
            rx_options.shift = (int32_t)atoi(optarg);
            break;
        case 'u': // Upconverter frequency
            rx_options.upconverter = (uint32_t)atofs(optarg);
            break;
        case 'k': // Bit packing
            rx_options.packing = (uint32_t)atoi(optarg);
            if (rx_options.packing <= 0) rx_options.packing = 0;
            if (rx_options.packing >= 1) rx_options.packing = 1;
            break;
        case 'H': // Decoder option, use a hastable
            dec_options.usehashtable = 0;
            break;
        case 'Q': // Decoder option, faster
            dec_options.quickmode = 1;
            break;
        case 'S': // Decoder option, single pass mode (same as original wsprd)
            dec_options.subtraction = 0;
            dec_options.npasses = 1;
            break;
        default:
            usage();
            break;
        }
    }

    if (dec_options.rcall[0] == 0) {
        fprintf(stderr, "Please specify your callsign.\n");
    }

    if (dec_options.rloc[0] == 0) {
        fprintf(stderr, "Please specify your locator.\n");
    }

    /* Calcule decimation rate & frequency offset for fs/4 shift */
    rx_options.realfreq =  wsprfrsets[rx_options.fset].centerfr + rx_options.shift + rx_options.upconverter;

    /* init channels data */
    for(n=0;n<wsprfrsets[rx_options.fset].nbfr;n++) {
	channels[n].decode_flag = true;
	channels[n].fr = wsprfrsets[rx_options.fset].fr[n];

    	channels[n].mixerphase=0;

    	channels[n].Ix=0;
    	channels[n].Qx=0;
    	channels[n].avridx=0;

    	channels[n].polyphase=0;

	channels[n].sigbuff_head=channels[n].sigbuff_tail=NULL;
    	pthread_cond_init(&(channels[n].sigbuff_cond), NULL);
    	pthread_mutex_init(&(channels[n].sigbuff_mutex), NULL);

    	genosc(rx_options.fset,n,&(channels[n]));

    	pthread_create(&(channels[n].wthread), NULL, wsprDecoder, &(channels[n]));
    }

    /* If something goes wrong... */
    signal(SIGINT, &sigint_callback_handler);
    signal(SIGFPE, &sigint_callback_handler);
    signal(SIGTERM, &sigint_callback_handler);

    if(dec_options.usehashtable)
	loadHashtable();

    /* Print used parameter */
    time_t rawtime;
    time ( &rawtime );
    struct tm *gtm = gmtime(&rawtime);
    printf("\nStarting airspy-wsprd (%04d-%02d-%02d, %02d:%02dz) -- Version 0.2\n",
           gtm->tm_year + 1900, gtm->tm_mon + 1, gtm->tm_mday, gtm->tm_hour, gtm->tm_min);
    printf("  Callsign     : %s\n", dec_options.rcall);
    printf("  Locator      : %s\n", dec_options.rloc);
    printf("  Real freq.   : %d Hz\n", rx_options.realfreq);
    printf("  Gain         : %d\n", rx_options.linearitygain);
    printf("  Bias         : %s\n", rx_options.bias ? "yes" : "no");
    printf("  Bits packing : %s\n", rx_options.packing ? "yes" : "no");

    initWsprNet();

    if(startairspy()) {
        	printf("Could not start airspy\n");
		exit(1);
    }

    /* Main loop : Wait, read, decode */
    while (!dec_options.exit_flag) {
	int64_t usec,uwait;
	struct timespec tp;

        /* Wait for time Sync on 2 mins */
	clock_gettime(CLOCK_REALTIME, &tp);
        usec  = tp.tv_sec * 1000000 + tp.tv_nsec/1000;
        uwait = 119000000 - usec % 120000000 ;
	if(uwait<0) uwait+=120000000;

	if(fst) {
        	printf("First spot in %ds\n",(int)(uwait/1000000));
		fst = false ;
	}

        usleep(uwait);

        /* Start to store the samples */
        initSampleStorage();

        /* Use the Store the date at the begin of the frame */
        rawtime=time(NULL)+1;
	gtm = gmtime(&rawtime);
        sprintf(dec_options.date,"%02d%02d%02d", gtm->tm_year - 100, gtm->tm_mon + 1, gtm->tm_mday);
        sprintf(dec_options.uttime,"%02d%02d", gtm->tm_hour, gtm->tm_min);

        usleep(100000000);
    }

    stopairspy();

    if(dec_options.usehashtable)
	saveHashtable();

    printf("Bye!\n");

    stopWrprNet();

    pthread_exit(NULL);

    return exit_code;
}
