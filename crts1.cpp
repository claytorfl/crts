#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <math.h>
#include <complex>
#include <liquid/liquid.h>
// Definition of liquid_float_complex changes depending on
// whether <complex> is included before or after liquid.h
#include <liquid/ofdmtxrx.h>
#include <time.h>
#include <string.h>
// For Threading (POSIX Threads)
#include <pthread.h>
// For config file
#include <libconfig.h>

//TCP Header Files
#include <sys/socket.h> // for socket(), connect(), send(), and recv() 
#include <sys/types.h>
#include <arpa/inet.h>  // for sockaddr_in and inet_addr() 
#include <string.h>     // for memset() 
#include <unistd.h>     // for close() 
#include <errno.h>
#include <sys/types.h>  // for killing child process
#include <signal.h>     // for killing child process
#include <uhd/usrp/multi_usrp.hpp>
#include <getopt.h>     // For command line options
#define MAXPENDING 5

// SO_REUSEPORT is defined only defined with linux 3.10+.
// Makes compatible with earlier kernels.
#ifndef SO_REUSEPORT
#define SO_REUSEPORT SO_REUSEADDR
#endif


void usage() {
    printf("crts -- Test cognitive radio engines. Data is logged in the 'data' directory to a file named 'data_crts' with date and time appended.\n");
    printf("  -u,-h  :   usage/help\n");
    printf("  -q     :   quiet - do not print debug info\n");
    printf("  -v     :   verbose - print debug info to stdout (default)\n");
    printf("  -d     :   print data to stdout rather than to file (implies -q unless -v given)\n");
    printf("  -r     :   real transmissions using USRPs (opposite of -s)\n");
    printf("  -s     :   simulation mode (default)\n");
    printf("  -p     :   server port (default: 1402)\n");
    printf("  -c     :   controller - this crts instance will act as experiment controller (needs -r)\n");
    printf("  -a     :   server IP address (when not controller. default: 127.0.0.1)\n");
    printf("  -f     :   center frequency [Hz] (when not controller. default: 460 MHz)\n");
    printf("  -b     :   bandwidth [Hz], (when not controller. default: 1.0 MHz)\n");
    printf("  -G     :   uhd rx gain [dB] (when not controller. default: 20dB)\n");
    printf("  -M     :   number of subcarriers (when not controller. default: 64)\n");
    printf("  -C     :   cyclic prefix length (when not controller. default: 16)\n");
    printf("  -T     :   taper length (when not controller. default: 4)\n");
    //printf("  f     :   center frequency [Hz], default: 462 MHz\n");
    //printf("  b     :   bandwidth [Hz], default: 250 kHz\n");
    //printf("  G     :   uhd rx gain [dB] (default: 20dB)\n");
    //printf("  t     :   run time [seconds]\n");
    //printf("  z     :   number of subcarriers to notch in the center band, default: 0\n");
}






struct CognitiveEngine {
    char modScheme[30];
    char crcScheme[30];
    char innerFEC[30];
    char outerFEC[30];
    char outerFEC_prev[30];
    char adaptationCondition[30];
    float default_tx_power;
    char adaptation[30];
    char goal[30];
    float threshold;
    // TODO: For latestGoalValue, Use different type of variable depending on
    //  what its being compared to
    float latestGoalValue;
    float weightedAvg; 
    float PER;
    float BERLastPacket;
    float BERTotal;
    float frequency;
    float bandwidth;
    float txgain_dB;
    float uhd_txgain_dB;
    float delay_us;
    float weighted_avg_payload_valid_threshold;
    float PER_threshold;
    float BER_threshold;
    float FECswitch;
    double startTime;
    double runningTime; // In seconds
    int iteration;
	unsigned int bitsPerSym;
    unsigned int validPayloads;
    unsigned int errorFreePayloads;
    unsigned int frameNumber;
    unsigned int lastReceivedFrame;
    unsigned int payloadLen;
    unsigned int payloadLenIncrement;
    unsigned int payloadLenMax;
    unsigned int payloadLenMin;
    unsigned int numSubcarriers;
    unsigned int CPLen;
    unsigned int taperLen;
	unsigned int averaging;
	float metric_mem[100]; // For computing running average
	float averagedGoalValue;

};

struct Scenario {
    int addAWGNBaseband; //Does the Scenario have noise?
    float noiseSNR;
    float noiseDPhi;
    
    int addInterference; // Does the Scenario have interference?
    
    int addRicianFadingBaseband; // Does the Secenario have fading?
    float fadeK;
    float fadeFd;
    float fadeDPhi;

	int addCWInterfererBaseband; // Does the Scenario have a CW interferer?
	float cw_pow;
	float cw_freq;
};

struct rxCBstruct {
    unsigned int serverPort;
    int verbose;
    float bandwidth;
    char * serverAddr;
	msequence * rx_ms_ptr;
    int ce_num;
    int sc_num;
    int frameNum;
	int client;
	int primaryon;
	int secondarysending;
	char usrptype;
	int number;
};

struct feedbackStruct {
    int             header_valid;
    int             payload_valid;
    unsigned int    payload_len;
    unsigned int    payloadByteErrors;
    unsigned int    payloadBitErrors;
    unsigned int    iteration;
	//int				ce_num;
	//int				sc_num;
    float           evm;
    float           rssi;
    float           cfo;
	int				primaryon;
	int				primary;
	int				secondary;
	int 			block_flag;
};



//Structure for using TCP to pass both feedback structs and info on when primaries and secondaries
//turn on and off
struct message{
	std::clock_t timestamp;
	char type;
	char purpose;
	struct feedbackStruct feed;
	int number;
	int msgreceived;
	int client;
};

//Adds together the values of 2 feedback structures
struct feedbackStruct feedbackadder(struct feedbackStruct fb1, struct feedbackStruct fb2){
	struct feedbackStruct resultfb;
	resultfb.header_valid = fb1.header_valid + fb2.header_valid;
	resultfb.payload_valid = fb1.payload_valid + fb2.payload_valid;
   	resultfb.payload_len = fb1.payload_len + fb2.payload_len;
	resultfb.payloadByteErrors = fb1.payloadByteErrors + fb2.payloadByteErrors;
   	resultfb.payloadBitErrors = fb1.payloadBitErrors + fb2.payloadBitErrors;
	resultfb.iteration = fb1.iteration + fb2.iteration;
   	resultfb.evm = fb1.evm + fb2.evm;
	resultfb. rssi = fb1.rssi + fb2.rssi;
	resultfb.cfo = fb1.cfo + fb2.cfo;
	return resultfb;
}

//Finds the position of an integer in an array of ints
int finder(int * int_ptr, int * length, int value){
	int iterator;
	int len = *length;
	for(iterator=0; iterator<len; ++iterator){
		if(int_ptr[iterator] == value){
			return iterator;
		}
	}
	int_ptr[len] = value;
	*length = len + 1;
	return len;
}


struct serverThreadStruct {
    unsigned int serverPort;
    struct feedbackStruct * fb_ptr;
	char type;
	float * floatnumber;
	struct message * m_ptr;
};

struct serveClientStruct {
	int client;
	struct feedbackStruct * fb_ptr;
	float *floatnumber;
	struct message * m_ptr;
};

struct broadcastfeedbackinfo{
	struct message * m_ptr;
	int client;
	int * msgnumber;
};

struct scenarioSummaryInfo{
	int total_frames[60][60];
	int valid_headers[60][60];
	int valid_payloads[60][60];
	int total_bits[60][60];
	int bit_errors[60][60];
	float EVM[60][60];
	float RSSI[60][60];
	float PER[60][60];
};

struct cognitiveEngineSummaryInfo{
	int total_frames[60];
	int valid_headers[60];
	int valid_payloads[60];
	int total_bits[60];
	int bit_errors[60];
	float EVM[60];
	float RSSI[60];
	float PER[60];
};



// Default parameters for a Cognitive Engine
struct CognitiveEngine CreateCognitiveEngine() {
    struct CognitiveEngine ce = {};
    ce.default_tx_power = 10.0;
    ce.threshold = 1.0;                 // Desired value for goal
    ce.latestGoalValue = 0.0;           // Value of goal to be compared to threshold
    ce.iteration = 1;                 // Count of total simulations.
    ce.payloadLen = 120;                // Length of payload in frame generation
    ce.payloadLenIncrement = 2;         // How much to increment payload in adaptations
                                        // Always positive.

    ce.payloadLenMax = 500;             // Max length of payload in bytes
    ce.payloadLenMin = 20;              // Min length of payload in bytes
    ce.numSubcarriers = 64;             // Number of subcarriers for OFDM
    ce.CPLen = 16;                      // Cyclic Prefix length
    ce.taperLen = 4;                     // Taper length
    ce.weightedAvg = 0.0;
    ce.PER = 0.0;
    ce.BERLastPacket = 0.0;
    ce.BERTotal = 0.0;
	ce.bitsPerSym = 1;
    ce.frameNumber = 0;
    ce.lastReceivedFrame = 0;
    ce.validPayloads = 0;
    ce.errorFreePayloads = 0;
    ce.frequency = 460.0e6;
    ce.bandwidth = 1.0e6;
    ce.txgain_dB = -12.0;
    ce.uhd_txgain_dB = 40.0;
    ce.startTime = 0.0;
    ce.runningTime = 0.0; // In seconds
    ce.delay_us = 1000000.0; // In useconds
    ce.weighted_avg_payload_valid_threshold = 0.5;
    ce.PER_threshold = 0.5;
    ce.BER_threshold = 0.5;
    ce.FECswitch = 1;
	ce.averaging = 1;
	memset(ce.metric_mem,0,100*sizeof(float));
	ce.averagedGoalValue = 0;
    strcpy(ce.modScheme, "QPSK");
    strcpy(ce.adaptation, "mod_scheme->BPSK");
    strcpy(ce.goal, "payload_valid");
    strcpy(ce.crcScheme, "none");
    strcpy(ce.innerFEC, "none");
    strcpy(ce.outerFEC, "Hamming74");
    strcpy(ce.outerFEC_prev, "Hamming74");
    //strcpy(ce.adaptationCondition, "weighted_avg_payload_valid"); 
    strcpy(ce.adaptationCondition, "packet_error_rate"); 
    return ce;
} // End CreateCognitiveEngine()

// Default parameter for Scenario
struct Scenario CreateScenario() {
    struct Scenario sc = {};
    sc.addAWGNBaseband = 0,
    sc.noiseSNR = 7.0f, // in dB
    sc.noiseDPhi = 0.001f,

    sc.addInterference = 0,

    sc.addRicianFadingBaseband = 0,
    sc.fadeK = 30.0f,
    sc.fadeFd = 0.2f,
    sc.fadeDPhi = 0.001f;

	sc.addCWInterfererBaseband = 0;
	sc.cw_pow = 0;
	sc.cw_freq = 0;

    return sc;
} // End CreateScenario()

// Defaults for struct that is sent to rxCallBack()
struct rxCBstruct CreaterxCBStruct() {
    struct rxCBstruct rxCB = {};
    rxCB.serverPort = 1402;
    rxCB.bandwidth = 1.0e6;
    rxCB.serverAddr = (char*) "127.0.0.1";
    rxCB.verbose = 1;
	rxCB.number = 1;
    return rxCB;
} // End CreaterxCBStruct()

// Defaults for struct that is sent to server thread
struct serverThreadStruct CreateServerStruct() {
    struct serverThreadStruct ss = {};
    ss.serverPort = 1402;
    ss.fb_ptr = NULL;
    return ss;
} // End CreateServerStruct()

//Defaults for struct that is sent to client threads
struct serveClientStruct CreateServeClientStruct() {
	struct serveClientStruct sc = {};
	sc.client = 0;
	sc.fb_ptr = NULL;
	return sc;
}; // End CreateServeClientStruct



void feedbackStruct_print(feedbackStruct * fb_ptr)
{
    // TODO: make formatting nicer
    printf("feedbackStruct_print():\n");
    printf("\theader_valid:\t%d\n",       fb_ptr->header_valid);
    printf("\tpayload_valid:\t%d\n",      fb_ptr->header_valid);
    printf("\tpayload_len:\t%u\n",        fb_ptr->payload_len);
    printf("\tpayloadByteErrors:\t%u\n",  fb_ptr->payloadByteErrors);
    printf("\tpayloadBitErrors:\t%u\n",   fb_ptr->payloadBitErrors);
    printf("\tframeNum:\t%u\n",           fb_ptr->iteration);
    //printf("\tce_num:\t%d\n",             fb_ptr->ce_num);
    //printf("\tsc_num:\t%d\n",             fb_ptr->sc_num);
    printf("\tevm:\t%f\n",                fb_ptr->evm);
    printf("\trssi:\t%f\n",               fb_ptr->rssi);
    printf("\tcfo:\t%f\n",                fb_ptr->cfo);
}

int readScMasterFile(char scenario_list[30][60], int verbose )
{
    config_t cfg;                   // Returns all parameters in this structure 
    config_setting_t *setting;
    const char *str;                // Stores the value of the String Parameters in Config file
    int tmpI;                       // Stores the value of Integer Parameters from Config file                

    char current_sc[30];
    int no_of_scenarios=1;
    int i;
    char tmpS[30];
    //Initialization
    config_init(&cfg);
   
   
    // Read the file. If there is an error, report it and exit. 
    if (!config_read_file(&cfg,"master_scenario_file.txt"))
    {
        fprintf(stderr, "\n%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        fprintf(stderr, "\nCould not find master scenario file. It should be named 'master_scenario_file.txt'\n");
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    }
    else
        //printf("\nFound master Scenario config file\n")
        ;

  
    //// Get the configuration file name. 
    //if (config_lookup_string(&cfg, "filename", &str))
    //    //printf("File Type: %s\n", str)
    //    ;
    //else
    //    fprintf(stderr, "No 'filename' setting in configuration file.\n");

    // Read the parameter group
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        
        if (config_setting_lookup_int(setting, "NumberofScenarios", &tmpI))
        {
            no_of_scenarios=tmpI;
            if (verbose)
                printf ("Number of Scenarios: %d\n",tmpI);
        }
        
       for (i=1;i<=no_of_scenarios;i++)
       //while (strcmp (status,"end"!=0))
       {
         strcpy (current_sc,"scenario_");
         sprintf (tmpS,"%d",i);
         //printf ("\n Scenario Number =%s", tmpS);
         strcat (current_sc,tmpS);
         //printf ("\n CURRENT SCENARIO =%s", current_sc);
         if (config_setting_lookup_string(setting, current_sc, &str))
          {
              strcpy(*((scenario_list)+i-1),str);          
              //printf ("\nSTR=%s\n",str);
          }
        /*else
            printf("\nNo 'param2' setting in configuration file.");
          */
        if (verbose)
            printf ("Scenario File: %s\n", *((scenario_list)+i-1) );
      } 
    }
    config_destroy(&cfg);
    return no_of_scenarios;
} // End readScMasterFile()

int readCEMasterFile(char cogengine_list[30][60], int verbose)
{
    config_t cfg;               // Returns all parameters in this structure 
    config_setting_t *setting;
    const char *str;            // Stores the value of the String Parameters in Config file
    int tmpI;                   // Stores the value of Integer Parameters from Config file             

    char current_ce[30];
    int no_of_cogengines=1;
    int i;
    char tmpS[30];
    //Initialization
    config_init(&cfg);
   
    //printf ("\nInside readCEMasterFile function\n");
    //printf ("%sCogEngine List[0]:\n",cogengine_list[0] );

    // Read the file. If there is an error, report it and exit. 
    if (!config_read_file(&cfg,"master_cogengine_file.txt"))
    {
        fprintf(stderr, "\n%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        fprintf(stderr, "\nCould not find master file. It should be named 'master_cogengine_file.txt'\n");
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    }
    else
        //printf("Found master Cognitive Engine config file.\n");
        ;
  
    //// Get the configuration file name. 
    //if (config_lookup_string(&cfg, "filename", &str))
    //    //printf("File Type: %s\n", str)
    //    ;
    //else
    //    fprintf(stderr, "No 'filename' setting in master CE configuration file.\n");

    // Read the parameter group
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        if (config_setting_lookup_int(setting, "NumberofCogEngines", &tmpI))
        {
            no_of_cogengines=tmpI;
            if (verbose)
                printf ("Number of Congnitive Engines: %d\n",tmpI);
        }
        
        for (i=1;i<=no_of_cogengines;i++)
        {
            strcpy (current_ce,"cogengine_");
            sprintf (tmpS,"%d",i);
            //printf ("\n Scenario Number =%s", tmpS);
            strcat (current_ce,tmpS);
            //printf ("\n CURRENT SCENARIO =%s", current_sc);
            if (config_setting_lookup_string(setting, current_ce, &str))
            {
                strcpy(*((cogengine_list)+i-1),str);          
                //printf ("\nSTR=%s\n",str);
            }
            if (verbose)
                printf ("Cognitive Engine File: %s\n", *((cogengine_list)+i-1) );
        } 
    }
    config_destroy(&cfg);
    return no_of_cogengines;
} // End readCEMasterFile()

///////////////////Cognitive Engine///////////////////////////////////////////////////////////
////////Reading the cognitive radio parameters from the configuration file////////////////////
int readCEConfigFile(struct CognitiveEngine * ce, char *current_cogengine_file, int verbose)
{
    config_t cfg;               // Returns all parameters in this structure 
    config_setting_t *setting;
    const char * str;           // Stores the value of the String Parameters in Config file
    int tmpI;                   // Stores the value of Integer Parameters from Config file
    double tmpD;                
    char ceFileLocation[60];

    strcpy(ceFileLocation, "ceconfigs/");
    strcat(ceFileLocation, current_cogengine_file);

    if (verbose)
        printf("Reading ceconfigs/%s\n", current_cogengine_file);

    //Initialization
    config_init(&cfg);

    // Read the file. If there is an error, report it and exit. 
    if (!config_read_file(&cfg,ceFileLocation))
    {
        fprintf(stderr, "\n%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    }

    // Get the configuration file name. 
    //if (config_lookup_string(&cfg, "filename", &str))
    //{
    //    if (verbose)
    //        printf("Cognitive Engine Configuration File Name: %s\n", str);
    //}
    //else
    //    printf("No 'filename' setting in configuration file.\n");

    // Read the parameter group
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        // Read the strings
        if (config_setting_lookup_string(setting, "adaptation", &str))
        {
            strcpy(ce->adaptation,str);
            if (verbose) printf("Option to adapt: %s\n",str);
        }
       
        if (config_setting_lookup_string(setting, "goal", &str))
        {
            strcpy(ce->goal,str);
            if (verbose) printf("Goal: %s\n",str);
        }
        if (config_setting_lookup_string(setting, "adaptationCondition", &str))
        {
            strcpy(ce->adaptationCondition,str);
            if (verbose) printf("adaptationCondition: %s\n",str);
        }
        if (config_setting_lookup_string(setting, "modScheme", &str))
        {
            strcpy(ce->modScheme,str);
            if (verbose) printf("Modulation Scheme:%s\n",str);
        }
        if (config_setting_lookup_string(setting, "crcScheme", &str))
        {
            strcpy(ce->crcScheme,str);
            if (verbose) printf("CRC Scheme:%s\n",str);
        }
        if (config_setting_lookup_string(setting, "innerFEC", &str))
        {
            strcpy(ce->innerFEC,str);
            if (verbose) printf("Inner FEC Scheme:%s\n",str);
        }
        if (config_setting_lookup_string(setting, "outerFEC", &str))
        {
            strcpy(ce->outerFEC,str);
            if (verbose) printf("Outer FEC Scheme:%s\n",str);
        }

        // Read the integers
        if (config_setting_lookup_int(setting, "iterations", &tmpI))
        {
           //ce->iteration=tmpI;
           if (verbose) printf("Iterations: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "payloadLen", &tmpI))
        {
           ce->payloadLen=tmpI; 
           if (verbose) printf("PayloadLen: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "payloadLenIncrement", &tmpI))
        {
           ce->payloadLenIncrement=tmpI; 
           if (verbose) printf("PayloadLenIncrement: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "payloadLenMax", &tmpI))
        {
           ce->payloadLenMax=tmpI; 
           if (verbose) printf("PayloadLenMax: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "payloadLenMin", &tmpI))
        {
           ce->payloadLenMin=tmpI; 
           if (verbose) printf("PayloadLenMin: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "numSubcarriers", &tmpI))
        {
           ce->numSubcarriers=tmpI; 
           if (verbose) printf("Number of Subcarriers: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "CPLen", &tmpI))
        {
           ce->CPLen=tmpI; 
           if (verbose) printf("CPLen: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "taperLen", &tmpI))
        {
           ce->taperLen=tmpI; 
           if (verbose) printf("taperLen: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "delay_us", &tmpI))
        {
           ce->delay_us=tmpI; 
           if (verbose) printf("delay_us: %d\n", tmpI);
        }
        // Read the floats
        if (config_setting_lookup_float(setting, "default_tx_power", &tmpD))
        {
           ce->default_tx_power=tmpD; 
           if (verbose) printf("Default Tx Power: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "latestGoalValue", &tmpD))
        {
           ce->latestGoalValue=tmpD; 
           if (verbose) printf("Latest Goal Value: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "threshold", &tmpD))
        {
           ce->threshold=tmpD; 
           if (verbose) printf("Threshold: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "frequency", &tmpD))
        {
           ce->frequency=tmpD; 
           if (verbose) printf("frequency: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "txgain_dB", &tmpD))
        {
           ce->txgain_dB=tmpD; 
           if (verbose) printf("txgain_dB: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "bandwidth", &tmpD))
        {
           ce->bandwidth=tmpD; 
           if (verbose) printf("bandwidth: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "uhd_txgain_dB", &tmpD))
        {
           ce->uhd_txgain_dB=tmpD; 
           if (verbose) printf("uhd_txgain_dB: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "weighted_avg_payload_valid_threshold", &tmpD))
        {
           ce->weighted_avg_payload_valid_threshold=tmpD; 
           if (verbose) printf("weighted_avg_payload_valid_threshold: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "PER_threshold", &tmpD))
        {
           ce->PER_threshold=tmpD; 
           if (verbose) printf("PER_threshold: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "BER_threshold", &tmpD))
        {
           ce->BER_threshold=tmpD; 
           if (verbose) printf("BER_threshold: %f\n", tmpD);
        }
		if (config_setting_lookup_float(setting, "averaging", &tmpD))
        {
           ce->averaging=tmpD; 
           if (verbose) printf("Averaging: %f\n", tmpD);
        }
    }
    config_destroy(&cfg);
    return 1;
} // End readCEConfigFile()

int readScConfigFile(struct Scenario * sc, char *current_scenario_file, int verbose)
{
    config_t cfg;               // Returns all parameters in this structure 
    config_setting_t *setting;
    //const char * str;
    int tmpI;
    double tmpD;
    char scFileLocation[60];

    //printf("In readScConfigFile(): string current_scenario_file: \n%s\n", current_scenario_file);

    // Because the file is in the folder 'scconfigs'
    strcpy(scFileLocation, "scconfigs/");
    strcat(scFileLocation, current_scenario_file);
    //printf("In readScConfigFile(): string scFileLocation: \n%s\n", scFileLocation);

    // Initialization 
    config_init(&cfg);

    // Read the file. If there is an error, report it and exit. 
    if (!config_read_file(&cfg, scFileLocation))
    {
        fprintf(stderr, "%s:%d - %s\n", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    }

    //// Get the configuration file name. 
    //if (config_lookup_string(&cfg, "filename", &str))
    //    if (verbose)
    //        printf("File Name: %s\n", str);
    //else
    //    printf("No 'filename' setting in configuration file.\n");

    // Read the parameter group.
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        // Read the integer
        if (config_setting_lookup_int(setting, "addAWGNBaseband", &tmpI))
        {
            sc->addAWGNBaseband=tmpI;
            if (verbose) printf("addAWGNBaseband: %d\n", tmpI);
        }
        //else
        //    printf("No AddNoise setting in configuration file.\n");
        
        // Read the double
        if (config_setting_lookup_float(setting, "noiseSNR", &tmpD))
        {
            sc->noiseSNR=(float) tmpD;
            if (verbose) printf("Noise SNR: %f\n", tmpD);
        }
        //else
        //    printf("No Noise SNR setting in configuration file.\n");
       
        // Read the double
        if (config_setting_lookup_float(setting, "noiseDPhi", &tmpD))
        {
            sc->noiseDPhi=(float) tmpD;
            if (verbose) printf("NoiseDPhi: %f\n", tmpD);
        }
        //else
        //    printf("No NoiseDPhi setting in configuration file.\n");

        // Read the integer
        /*
        if (config_setting_lookup_int(setting, "addInterference", &tmpI))
        {
            sc->addInterference=tmpI;
            if (verbose) printf("addInterference: %d\n", tmpI);
        }
        //else
        //    printf("No addInterference setting in configuration file.\n");
        */

        // Read the integer
        if (config_setting_lookup_int(setting, "addRicianFadingBaseband", &tmpI))
        {
            sc->addRicianFadingBaseband=tmpI;
            if (verbose) printf("addRicianFadingBaseband: %d\n", tmpI);
        }
        //else
        //    printf("No addRicianFadingBaseband setting in configuration file.\n");

        // Read the double
        if (config_setting_lookup_float(setting, "fadeK", &tmpD))
        {
            sc->fadeK=(float)tmpD;
            if (verbose) printf("fadeK: %f\n", tmpD);
        }
        //else
        //    printf("No fadeK setting in configuration file.\n");

        // Read the double
        if (config_setting_lookup_float(setting, "fadeFd", &tmpD))
        {
            sc->fadeFd=(float)tmpD;
            if (verbose) printf("fadeFd: %f\n", tmpD);
        }
        //else
        //    printf("No fadeFd setting in configuration file.\n");

        // Read the double
        if (config_setting_lookup_float(setting, "fadeDPhi", &tmpD))
        {
            sc->fadeDPhi=(float)tmpD;
            if (verbose) printf("fadeDPhi: %f\n", tmpD);
        }

		// Read the integer
		if (config_setting_lookup_int(setting, "addCWInterfererBaseband", &tmpI))
        {
            sc->addCWInterfererBaseband=(float)tmpI;
            if (verbose) printf("addCWIntefererBaseband: %i\n", tmpI);
        }

		// Read the double
		if (config_setting_lookup_float(setting, "cw_pow", &tmpD))
        {
            sc->cw_pow=(float)tmpD;
            if (verbose) printf("cw_pow: %f\n", tmpD);
        }

		// Read the double
		if (config_setting_lookup_float(setting, "cw_freq", &tmpD))
        {
            sc->cw_freq=(float)tmpD;
            if (verbose) printf("cw_freq: %f\n", tmpD);
        }
        //else
        //    printf("No cw_freq setting in configuration file.\n");
    }

    config_destroy(&cfg);

    //printf("End of readScConfigFile() Function\n");
    return 1;
} // End readScConfigFile()

// Add AWGN
void enactAWGNBaseband(std::complex<float> * transmit_buffer, struct CognitiveEngine ce, struct Scenario sc)
{
    //options
    float dphi  = sc.noiseDPhi;                              // carrier frequency offset
    float SNRdB = sc.noiseSNR;                               // signal-to-noise ratio [dB]
    unsigned int symbol_len = ce.numSubcarriers + ce.CPLen;  // defining symbol length
    
    //printf("In enactAWGNBaseband: SNRdB=%f\n", SNRdB);

    // noise parameters
    float nstd = powf(10.0f, -SNRdB/20.0f); // noise standard deviation
    float phi = 0.0f;                       // channel phase
   
    std::complex<float> tmp (0, 1); 
    unsigned int i;

    // noise mixing
    for (i=0; i<symbol_len; i++) {
        transmit_buffer[i] = std::exp(tmp*phi) * transmit_buffer[i]; // apply carrier offset
        //transmit_buffer[i] *= cexpf(_Complex_I*phi); // apply carrier offset
        phi += dphi;                                 // update carrier phase
        cawgn(&transmit_buffer[i], nstd);            // add noise
    }
} // End enactAWGNBaseband()

void enactCWInterfererBaseband(std::complex<float> * transmit_buffer, struct CognitiveEngine ce, struct Scenario sc)
{
	float fs = ce.bandwidth; // Sample rate of the transmit buffer
	float k = pow(10.0, sc.cw_pow/20.0); // Coefficient to set the interferer power correctly
	unsigned int symbol_len = ce.numSubcarriers + ce.CPLen;  // defining symbol length

	//printf("Coefficient: %f\n", k);

	for(unsigned int i=0; i<symbol_len; i++)
	{
		transmit_buffer[i] += k*sin(6.283*sc.cw_freq*i/fs); // Add CW tone
	}
} // End enactCWInterfererBaseband()

// Add Rice-K Fading
void enactRicianFadingBaseband(std::complex<float> * transmit_buffer, struct CognitiveEngine ce, struct Scenario sc)
{
    // options
    unsigned int symbol_len = ce.numSubcarriers + ce.CPLen; // defining symbol length
    unsigned int h_len;                                     // doppler filter length
    if (symbol_len > 94){
        h_len = 0.0425*symbol_len;
    }
    else {
        h_len = 4;
    }
    float fd           = sc.fadeFd; // maximum doppler frequency
    float K            = sc.fadeK;  // Rice fading factor
    float omega        = 1.0f;      // mean power
    float theta        = 0.0f;      // angle of arrival
    float dphi = sc.fadeDPhi;       // carrier frequency offset
    float phi = 0.0f;               // channel phase

    // validate input
    if (K < 1.5f) {
        fprintf(stderr, "error: fading factor K must be greater than 1.5\n");
        exit(1);
    } else if (omega < 0.0f) {
        fprintf(stderr, "error: signal power Omega must be greater than zero\n");
        exit(1);
    } else if (fd <= 0.0f || fd >= 0.5f) {
        fprintf(stderr, "error: Doppler frequency must be in (0,0.5)\n");
        exit(1);
    } else if (symbol_len== 0) {
        fprintf(stderr, "error: number of samples must be greater than zero\n");
        exit(1);
    }
 
    unsigned int i;

    // allocate array for output samples
    std::complex<float> * y = (std::complex<float> *) malloc(symbol_len*sizeof(std::complex<float>));
    // generate Doppler filter coefficients
    float h[h_len];
    liquid_firdes_doppler(h_len, fd, K, theta, h);

    // normalize filter coefficients such that output Gauss random
    // variables have unity variance
    float std = 0.0f;
    for (i=0; i<h_len; i++)
        std += h[i]*h[i];
    std = sqrtf(std);
    for (i=0; i<h_len; i++)
        h[i] /= std;

    // create Doppler filter from coefficients
    firfilt_crcf fdoppler = firfilt_crcf_create(h,h_len);

    // generate complex circular Gauss random variables
    std::complex<float> v;    // circular Gauss random variable (uncorrelated)
    std::complex<float> x;    // circular Gauss random variable (correlated w/ Doppler filter)
    float s   = sqrtf((omega*K)/(K+1.0));
    float sig = sqrtf(0.5f*omega/(K+1.0));
        
    std::complex<float> tmp(0, 1);
    for (i=0; i<symbol_len; i++) {
        // generate complex Gauss random variable
        crandnf(&v);

        // push through Doppler filter
        firfilt_crcf_push(fdoppler, v);
        firfilt_crcf_execute(fdoppler, &x);

        // convert result to random variable with Rice-K distribution
        y[i] = tmp*( std::imag(x)*sig + s ) +
                          ( std::real(x)*sig     );
    }
    for (i=0; i<symbol_len; i++) {
        transmit_buffer[i] *= std::exp(tmp*phi);  // apply carrier offset
        phi += dphi;                                  // update carrier phase
        transmit_buffer[i] *= y[i];                   // apply Rice-K distribution
    }

    // destroy filter object
    firfilt_crcf_destroy(fdoppler);

    // clean up allocated array
    free(y);
} // End enactRicianFadingBaseband()

// Enact Scenario
void enactScenarioBaseband(std::complex<float> * transmit_buffer, struct CognitiveEngine ce, struct Scenario sc)
{
    // Add appropriate RF impairments for the scenario
    if (sc.addRicianFadingBaseband == 1)
    {
        enactRicianFadingBaseband(transmit_buffer, ce, sc);
    }
    if (sc.addCWInterfererBaseband == 1)
    {
        //fprintf(stderr, "WARNING: There is currently no interference scenario functionality!\n");
        // Interference function
        enactCWInterfererBaseband(transmit_buffer, ce, sc);
    }
    if (sc.addAWGNBaseband == 1)
    {
        enactAWGNBaseband(transmit_buffer, ce, sc);
    }
    if ( (sc.addAWGNBaseband == 0) && (sc.addCWInterfererBaseband == 0) && (sc.addRicianFadingBaseband == 0))
    {
       	fprintf(stderr, "WARNING: Nothing Added by Scenario!\n");
		//fprintf(stderr, "addCWInterfererBaseband: %i\n", sc.addCWInterfererBaseband);
    }
} // End enactScenarioBaseband()

void * call_uhd_siggen(void * param)
{

    return NULL;
} // end call_uhd_siggen()

/*
void enactUSRPScenario(struct CognitiveEngine ce, struct Scenario sc, pid_t* siggen_pid)
{
    // Check AWGN
    if (sc.addAWGNBaseband == 1){
       // Center freq of noise
       char freq_opt[40] = "-f";
       char * freq = NULL;
       sprintf(freq, "%f", ce.frequency);
       strcat(freq_opt, freq);

       // Type of output
       char output_opt[20] = "--gaussian";

       // Gain of Output
       char gain_opt[40] = "-g";
       char * noiseGain_dB = NULL;
       sprintf(noiseGain_dB, "%f", 10.0);
       strcat(gain_opt, noiseGain_dB);


       //pthread_create( siggenThread_ptr, NULL, call_uhd_siggen, NULL);
       *siggen_pid = fork();
       if ( *siggen_pid == -1 )
       {
           fprintf(stderr, "ERROR: Failed to fork child for uhd_siggen.\n" );
           _exit(EX_OSERR);
       }
       // If this is the child process
       if (*siggen_pid == 0)
       {
           // TODO: external call to uhd_siggen
           //system("/usr/bin/uhd_siggen_gui");
           execl("/usr/bin/uhd_siggen", "uhd_siggen", freq_opt, output_opt, gain_opt, (char*)NULL);
           perror("Error");
           // Then wait to be killed.
           while (1) {;}
       }
       // Give uhd_siggen time to initialize 
       sleep(8);

       //printf("WARNING: There is currently no USRP AWGN scenario functionality!\n");
       // FIXME: This is just test code. Remove when done.
           printf("siggen_pid= %d\n", *siggen_pid);
           kill(*siggen_pid, SIGKILL);
           //printf("ERROR: %s\n", strerror(errno));
           //perror("Error");
           while(1) {;}
    }
    if (sc.addInterference == 1){
       fprintf(stderr, "WARNING: There is currently no USRP interference scenario functionality!\n");
       // Interference function
    }
    if (sc.addRicianFadingBaseband == 1){
       //enactRicianFadingBaseband(transmit_buffer, ce, sc);
       fprintf(stderr, "WARNING: There is currently no USRP Fading scenario functionality!\n");
    }
    if ( (sc.addAWGNBaseband == 0) && (sc.addCWInterfererBaseband == 0) && (sc.addRicianFadingBaseband == 0) ){
       fprintf(stderr, "WARNING: Nothing Added by Scenario!\n");
    }
} // End enactUSRPScenario()
*/

modulation_scheme convertModScheme(char * modScheme, unsigned int * bps)
{
    modulation_scheme ms;
    // TODO: add other liquid-supported mod schemes
    if (strcmp(modScheme, "QPSK") == 0) {
        ms = LIQUID_MODEM_QPSK;
		*bps = 2;
    }
    else if ( strcmp(modScheme, "BPSK") ==0) {
        ms = LIQUID_MODEM_BPSK;
		*bps = 1;
    }
    else if ( strcmp(modScheme, "OOK") ==0) {
        ms = LIQUID_MODEM_OOK;
		*bps = 1;
    }
    else if ( strcmp(modScheme, "8PSK") ==0) {
        ms = LIQUID_MODEM_PSK8;
		*bps = 3;
    }
    else if ( strcmp(modScheme, "16PSK") ==0) {
        ms = LIQUID_MODEM_PSK16;
		*bps = 4;
    }
    else if ( strcmp(modScheme, "32PSK") ==0) {
        ms = LIQUID_MODEM_PSK32;
		*bps = 5;
    }
    else if ( strcmp(modScheme, "64PSK") ==0) {
        ms = LIQUID_MODEM_PSK64;
		*bps = 6;
    }
    else if ( strcmp(modScheme, "128PSK") ==0) {
        ms = LIQUID_MODEM_PSK128;
		*bps = 7;
    }
    else if ( strcmp(modScheme, "8QAM") ==0) {
        ms = LIQUID_MODEM_QAM8;
		*bps = 3;
    }
    else if ( strcmp(modScheme, "16QAM") ==0) {
        ms = LIQUID_MODEM_QAM16;
		*bps = 4;
    }
    else if ( strcmp(modScheme, "32QAM") ==0) {
        ms = LIQUID_MODEM_QAM32;
		*bps = 5;
    }
    else if ( strcmp(modScheme, "64QAM") ==0) {
        ms = LIQUID_MODEM_QAM64;
		*bps = 6;
    }
    else if ( strcmp(modScheme, "BASK") ==0) {
        ms = LIQUID_MODEM_ASK2;
		*bps = 1;
    }
    else if ( strcmp(modScheme, "4ASK") ==0) {
        ms = LIQUID_MODEM_ASK4;
		*bps = 2;
    }
    else if ( strcmp(modScheme, "8ASK") ==0) {
        ms = LIQUID_MODEM_ASK8;
		*bps = 3;
    }
    else if ( strcmp(modScheme, "16ASK") ==0) {
        ms = LIQUID_MODEM_ASK16;
		*bps = 4;
    }
    else if ( strcmp(modScheme, "32ASK") ==0) {
        ms = LIQUID_MODEM_ASK32;
		*bps = 5;
    }
    else if ( strcmp(modScheme, "64ASK") ==0) {
        ms = LIQUID_MODEM_ASK64;
		*bps = 6;
    }
    else if ( strcmp(modScheme, "128ASK") ==0) {
        ms = LIQUID_MODEM_ASK128;
		*bps = 7;
    }
    else {
        fprintf(stderr, "ERROR: Unknown Modulation Scheme");
        exit(EXIT_FAILURE);
        //TODO: Rather than halt execution,
        // Skip current test if given an unknown parameter.
    }

    return ms;
} // End convertModScheme()

crc_scheme convertCRCScheme(char * crcScheme, int verbose)
{
    crc_scheme check;
    if (strcmp(crcScheme, "none") == 0) {
        check = LIQUID_CRC_NONE;
        if (verbose) printf("check = LIQUID_CRC_NONE\n");
    }
    else if (strcmp(crcScheme, "checksum") == 0) {
        check = LIQUID_CRC_CHECKSUM;
        if (verbose) printf("check = LIQUID_CRC_CHECKSUM\n");
    }
    else if (strcmp(crcScheme, "8") == 0) {
        check = LIQUID_CRC_8;
        if (verbose) printf("check = LIQUID_CRC_8\n");
    }
    else if (strcmp(crcScheme, "16") == 0) {
        check = LIQUID_CRC_16;
        if (verbose) printf("check = LIQUID_CRC_16\n");
    }
    else if (strcmp(crcScheme, "24") == 0) {
        check = LIQUID_CRC_24;
        if (verbose) printf("check = LIQUID_CRC_24\n");
    }
    else if (strcmp(crcScheme, "32") == 0) {
        check = LIQUID_CRC_32;
        if (verbose) printf("check = LIQUID_CRC_32\n");
    }
    else {
        fprintf(stderr, "ERROR: unknown CRC\n");
        exit(EXIT_FAILURE);
        //TODO: Rather than halt execution,
        // Skip current test if given an unknown parameter.
    }

    return check;
} // End convertCRCScheme()

fec_scheme convertFECScheme(char * FEC, int verbose)
{
    // TODO: add other liquid-supported FEC schemes
    fec_scheme fec;
    if (strcmp(FEC, "none") == 0) {
        fec = LIQUID_FEC_NONE;
        if (verbose) printf("fec = LIQUID_FEC_NONE\n");
    }
    else if (strcmp(FEC, "Hamming74") == 0) {
        fec = LIQUID_FEC_HAMMING74;
        if (verbose) printf("fec = LIQUID_FEC_HAMMING74\n");
    }
    else if (strcmp(FEC, "Hamming128") == 0) {
        fec = LIQUID_FEC_HAMMING128;
        if (verbose) printf("fec = LIQUID_FEC_HAMMING128\n");
    }
    else if (strcmp(FEC, "Golay2412") == 0) {
        fec = LIQUID_FEC_GOLAY2412;
        if (verbose) printf("fec = LIQUID_FEC_GOLAY2412\n");
    }
    else if (strcmp(FEC, "SEC-DED2216") == 0) {
        fec = LIQUID_FEC_SECDED2216;
        if (verbose) printf("fec = LIQUID_FEC_SECDED2216\n");
    }
    else if (strcmp(FEC, "SEC-DED3932") == 0) {
        fec = LIQUID_FEC_SECDED3932;
        if (verbose) printf("fec = LIQUID_FEC_SECDED3932\n");
    }
    else if (strcmp(FEC, "SEC-DED7264") == 0) {
        fec = LIQUID_FEC_SECDED7264;
        if (verbose) printf("fec = LIQUID_FEC_SECDED7264\n");
    }
    else {
        fprintf(stderr, "ERROR: unknown FEC\n");
        exit(EXIT_FAILURE);
        //TODO: Rather than halt execution,
        // Skip current test if given an unknown parameter.
    }
    return fec;
} // End convertFECScheme()

// Create Frame generator with CE and Scenario parameters
ofdmflexframegen CreateFG(struct CognitiveEngine ce, struct Scenario sc, int verbose) {

    //printf("Setting inital ofdmflexframegen options:\n");
    // Set Modulation Scheme
    if (verbose) printf("Modulation scheme: %s\n", ce.modScheme);
    modulation_scheme ms = convertModScheme(ce.modScheme, &ce.bitsPerSym);

    // Set Cyclic Redundency Check Scheme
    crc_scheme check = convertCRCScheme(ce.crcScheme, verbose);

    // Set inner forward error correction scheme
    if (verbose) printf("Inner FEC: ");
    fec_scheme fec0 = convertFECScheme(ce.innerFEC, verbose);

    // Set outer forward error correction scheme
    // TODO: add other liquid-supported FEC schemes
    if (verbose) printf("Outer FEC: ");
    fec_scheme fec1 = convertFECScheme(ce.outerFEC, verbose);

    // Frame generation parameters
    ofdmflexframegenprops_s fgprops;

    // Initialize Frame generator and Frame Synchronizer Objects
    ofdmflexframegenprops_init_default(&fgprops);
    fgprops.mod_scheme      = ms;
    fgprops.check           = check;
    fgprops.fec0            = fec0;
    fgprops.fec1            = fec1;
    //printf("About to create fg...\n");
    ofdmflexframegen fg = ofdmflexframegen_create(ce.numSubcarriers, ce.CPLen, ce.taperLen, NULL, &fgprops);
    //printf("fg created\n");

    return fg;
} // End CreateFG()

int rxCallback(unsigned char *  _header,
               int              _header_valid,
               unsigned char *  _payload,
               unsigned int     _payload_len,
               int              _payload_valid,
               framesyncstats_s _stats,
               void *           _userdata)
{
    struct rxCBstruct * rxCBS_ptr = (struct rxCBstruct *) _userdata;
    int verbose = rxCBS_ptr->verbose;
	msequence rx_ms = *rxCBS_ptr->rx_ms_ptr; 

    // Variables for checking number of errors 
    unsigned int payloadByteErrors  =   0;
    unsigned int payloadBitErrors   =   0;
    int j,m;
	unsigned int tx_byte;

    // Calculate byte error rate and bit error rate for payload
    for (m=0; m<(signed int)_payload_len; m++)
    {
		tx_byte = msequence_generate_symbol(rx_ms,8);
		//printf( "%1i %1i\n", (signed int)_payload[m], tx_byte );
        if (((int)_payload[m] != tx_byte))
        {
            payloadByteErrors++;
            for (j=0; j<8; j++)
            {
				if ((_payload[m]&(1<<j)) != (tx_byte&(1<<j)))
                   payloadBitErrors++;
            }      
        }           
    }               
                    
    // Data that will be sent to server
    // TODO: Send other useful data through feedback array
	//printf("CALLBACK!!!\n\n");
    struct feedbackStruct fb = {};
    fb.header_valid         =   _header_valid;
    fb.payload_valid        =   _payload_valid;
    fb.payload_len          =   _payload_len;
    fb.payloadByteErrors    =   payloadByteErrors;
    fb.payloadBitErrors     =   payloadBitErrors;
    fb.evm                  =   _stats.evm;
    fb.rssi                 =   _stats.rssi;
    fb.cfo                  =   _stats.cfo;	
	//fb.ce_num				=	_header[0];
	//fb.sc_num				=	_header[1];
	fb.iteration			=	0;
	fb.block_flag			=	0;

	for(int i=0; i<4; i++)	fb.iteration += _header[i+2]<<(8*(3-i));

    if (verbose)
    {
        printf("In rxcallback():\n");
        printf("Header: %i %i %i %i %i %i %i %i\n", _header[0], _header[1], 
            _header[2], _header[3], _header[4], _header[5], _header[6], _header[7]);
        feedbackStruct_print(&fb);
    }

    // Receiver sends data to server
    write(rxCBS_ptr->client, (void*)&fb, sizeof(fb));

    return 0;

} // end rxCallback()

ofdmflexframesync CreateFS(struct CognitiveEngine ce, struct Scenario sc, struct rxCBstruct* rxCBs_ptr)
{
     ofdmflexframesync fs =
             ofdmflexframesync_create(ce.numSubcarriers, ce.CPLen, ce.taperLen, NULL, rxCallback, (void *) rxCBs_ptr);

     return fs;
} // End CreateFS();

// Transmit a packet of data.
// This will need to be modified once we implement the USRPs.
//int txGeneratePacket(struct CognitiveEngine ce, ofdmflexframegen * _fg, unsigned char * header, unsigned char * payload)
//{
//    return 1;
//} // End txGeneratePacket()

//int txTransmitPacket(struct CognitiveEngine ce, ofdmflexframegen * _fg, std::complex<float> * frameSamples, 
//                        uhd::tx_metadata_t md, uhd::tx_streamer::sptr txStream, int usingUSRPs)
//{
//    int isLastSymbol = ofdmflexframegen_writesymbol(*_fg, frameSamples);
//
//    return isLastSymbol;
//} // End txTransmitPacket()

// TODO: Alter code for when usingUSRPs
//int rxReceivePacket(struct CognitiveEngine ce, ofdmflexframesync * _fs, std::complex<float> * frameSamples, int usingUSRPs)
//{
//    unsigned int symbolLen = ce.numSubcarriers + ce.CPLen;
//    ofdmflexframesync_execute(*_fs, frameSamples, symbolLen);
//    return 1;
//} // End rxReceivePacket()

void * serveTCPclient(void * _sc_ptr){
	struct serveClientStruct * sc_ptr = (struct serveClientStruct*) _sc_ptr;
	struct feedbackStruct read_buffer;
	struct feedbackStruct *fb_ptr = sc_ptr->fb_ptr;
	while(1){
        bzero(&read_buffer, sizeof(read_buffer));
        read(sc_ptr->client, &read_buffer, sizeof(read_buffer));
		if (read_buffer.evm && !fb_ptr->block_flag) {*fb_ptr = read_buffer; fb_ptr->block_flag = 1;}
    }
    return NULL;
}


//Reads the messages from a TCP link to a DSA congnitive radio
void * serveTCPDSAclient(void * _sc_ptr){
	//int latestprimary = 0;
	//int latestsecondary = 0;
	int number = 0;
	int primarytest = 0;
	struct serveClientStruct * sc_ptr = (struct serveClientStruct*) _sc_ptr;
	int client = sc_ptr->client;
	struct message read_buffer;
	struct message *m_ptr = sc_ptr->m_ptr;
	while(1){
		if(m_ptr->msgreceived == 0){
		    bzero(&read_buffer, sizeof(read_buffer));
		    read(client, &read_buffer, sizeof(read_buffer));
			if(read_buffer.number > number and (read_buffer.type == 'p' or read_buffer.type == 's' or read_buffer.type == 'P' or read_buffer.type == 'S')){
				*m_ptr = read_buffer;
				m_ptr->msgreceived = 1;
				number = read_buffer.number;
			}
		}
		//if (read_buffer && !fb_ptr->block_flag) {*fb_ptr->evm = read_buffer; fb_ptr->block_flag = 1;}
    }
    return NULL;
}

void * feedbackThread(void * v_ptr){
	struct broadcastfeedbackinfo * bfi_ptr = (struct broadcastfeedbackinfo*) v_ptr;
	struct message * m_ptr = bfi_ptr->m_ptr;
	int client = bfi_ptr->client;
	//printf("%d\n", client);
	struct message msg;
	msg.type = 'P';
	int clientlist[10];
	clientlist[0] = 0;
	int clientlistlength = 1;
	int totalcycles = 0;
	int index = 0;
	int fblistlength = 10;
	//struct feedbackStruct fblist[fblistlength];
	//int feedbacknum[fblistlength];
	struct feedbackStruct basicfb;
	int fbnum = 1;

	//Zeroes out the feedback structures so they can be added to when
	//new feedback is received
	for(int o; o<fblistlength; ++o){
		/*fblist[o].header_valid = 0;
		fblist[o].payload_valid = 0;
	   	fblist[o].payload_len = 0;
		fblist[o].payloadByteErrors = 0;
	   	fblist[o].payloadBitErrors = 0;
		fblist[o].iteration = 0;
	   	fblist[o].evm = 0.0;
		fblist[o]. rssi = 0.0;
		fblist[o].cfo = 0.0;
		fblist[o].block_flag = 0;*/
		basicfb.header_valid = 0;
		basicfb.payload_valid = 0;
	   	basicfb.payload_len = 0;
		basicfb.payloadByteErrors = 0;
	   	basicfb.payloadBitErrors = 0;
		basicfb.iteration = 0;
	   	basicfb.evm = 0.0;
		basicfb. rssi = 0.0;
		basicfb.cfo = 0.0;
		basicfb.block_flag = 0;
		//feedbacknum[o] = 0;
	}
	int primary = 0;
	int secondary = 0;
	std::clock_t time = std::clock();
	int loop = 1;
	int h;
	
	while(loop){
		//printf("%d\n", *bfi_ptr->msgnumber);
		if(m_ptr->msgreceived == 1){
			if(m_ptr->type == 'p'){
				//index = finder(clientlist, &clientlistlength, msg.client); 
				if(m_ptr->purpose == 'P'){
					//fblist[index] = feedbackadder(fblist[index], msg.feed);
					time = std::clock();
					printf("Primary receiver received primary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
					primary++;
				}
				if(m_ptr->purpose == 'S'){;
					time = std::clock();
					printf("Primary receiver received secondary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
					secondary++;
				}
				if(m_ptr->purpose == 'f'){;
					time = std::clock();
					//printf("Received feedback from primary receiver with primary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
					primary++;
					for(h = 0; h<clientlistlength; h++){
						if(clientlist[h] == m_ptr->client){
							break;
						}
					}
					//index = std::find(clientlist, clientlistlength, m_ptr->client);
					if(h == clientlistlength){
						fbnum++;
						basicfb = feedbackadder(basicfb, m_ptr->feed);
						clientlist[clientlistlength] = m_ptr->client;
						clientlistlength++;
						}
					else{
						//printf("%d\n", client);
						basicfb.header_valid /= fbnum;
						basicfb.payload_valid /= fbnum;
					   	basicfb.payload_len /= fbnum;
						basicfb.payloadByteErrors /= fbnum;
					   	basicfb.payloadBitErrors /= fbnum;
						basicfb.iteration /= fbnum;
					   	basicfb.evm /= fbnum;
						basicfb. rssi /= fbnum;
						basicfb.cfo /= fbnum;
						basicfb.block_flag /= fbnum;
						msg.feed = basicfb;
						msg.purpose = 'f';
						msg.number = *bfi_ptr->msgnumber;
						//printf("%d\n", msg.number);
						write(client, (const void *)&msg, sizeof(msg));
						(*bfi_ptr->msgnumber)++;
						basicfb.header_valid = 0;
						basicfb.payload_valid = 0;
					   	basicfb.payload_len = 0;
						basicfb.payloadByteErrors = 0;
					   	basicfb.payloadBitErrors = 0;
						basicfb.iteration = 0;
					   	basicfb.evm = 0.0;
						basicfb. rssi = 0.0;
						basicfb.cfo = 0.0;
						basicfb.block_flag = 0;	
						basicfb = feedbackadder(basicfb, m_ptr->feed);			
						fbnum = 1;
						clientlist[0] = m_ptr->client;
						clientlistlength = 1;
					}
						
				}
				if(m_ptr->purpose == 'F'){;
					time = std::clock();
					printf("Received feedback from primary receiver with secondary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
					secondary++;
				}
			}
		m_ptr->msgreceived = 0;
		}
	};
	printf("Testing Complete\n");
	return NULL;
	
}
	


// Create a TCP socket for the server and bind it to a port
// Then sit and listen/accept all connections and write the data
// to an array that is accessible to the CE
void * startTCPServer(void * _ss_ptr)
{

    //printf("(Server thread called.)\n");

    struct serverThreadStruct * ss_ptr = (struct serverThreadStruct*) _ss_ptr;

    //  Local (server) address
    struct sockaddr_in servAddr;   
    // Parameters of client connection
    struct sockaddr_in clientAddr;              // Client address 
    socklen_t client_addr_size;  // Client address size
    int socket_to_client = -1;
    int reusePortOption = 1;

	pthread_t TCPServeClientThread[5]; // Threads for clients
	int client = 0; // Client counter
        
    // Create socket for incoming connections 
    int sock_listen;
    if ((sock_listen = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        fprintf(stderr, "Transmitter Failed to Create Server Socket.\n");
        exit(EXIT_FAILURE);
    }

    // Allow reuse of a port. See http://stackoverflow.com/questions/14388706/socket-options-so-reuseaddr-and-so-reuseport-how-do-they-differ-do-they-mean-t
    if (setsockopt(sock_listen, SOL_SOCKET, SO_REUSEPORT, (void*) &reusePortOption, sizeof(reusePortOption)) < 0 )
    {
        fprintf(stderr, " setsockopt() failed\n");
        exit(EXIT_FAILURE);
    }

    // Construct local (server) address structure 
    memset(&servAddr, 0, sizeof(servAddr));       // Zero out structure 
    servAddr.sin_family = AF_INET;                // Internet address family 
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY); // Any incoming interface 
    servAddr.sin_port = htons(ss_ptr->serverPort);              // Local port 
    // Bind to the local address to a port
    if (bind(sock_listen, (struct sockaddr *) &servAddr, sizeof(servAddr)) < 0)
    {
        fprintf(stderr, "ERROR: bind() error\n");
        exit(EXIT_FAILURE);
    }

    // Listen and accept connections indefinitely
    while (1)
    {
        // listen for connections on socket
        if (listen(sock_listen, MAXPENDING) < 0)
        {
            fprintf(stderr, "ERROR: Failed to Set Sleeping (listening) Mode\n");
            exit(EXIT_FAILURE);
        }
        //printf("\n(Server is now in listening mode)\n");

        // Accept a connection from client
        //printf("Server is waiting to accept connection from client\n");
        socket_to_client = accept(sock_listen, (struct sockaddr *)&clientAddr, &client_addr_size);
        //printf("socket_to_client= %d\n", socket_to_client);
        if(socket_to_client< 0)
        {
            fprintf(stderr, "ERROR: Sever Failed to Connect to Client\n");
            exit(EXIT_FAILURE);
        }
		// Create separate thread for each client as they are accepted.
		else {
			struct serveClientStruct sc = CreateServeClientStruct();
			sc.client = socket_to_client;
			sc.fb_ptr = ss_ptr->fb_ptr;
			sc.floatnumber = ss_ptr->floatnumber;
			sc.m_ptr = ss_ptr->m_ptr;
			if(ss_ptr->type == 'd'){
			pthread_create( &TCPServeClientThread[client], NULL, serveTCPDSAclient, (void*) &sc);
			}
			else{
			pthread_create( &TCPServeClientThread[client], NULL, serveTCPclient, (void*) &sc);
			}

		}
        //printf("Server has accepted connection from client\n");
	}// End While loop
	
} // End startTCPServer()

int ceProcessData(struct CognitiveEngine * ce, struct feedbackStruct * fbPtr, int verbose)
{
    if (verbose)
    {
        printf("In ceProcessData():\n");
        feedbackStruct_print(fbPtr);
    }

    ce->validPayloads += fbPtr->payload_valid;

    if (fbPtr->payload_valid && (!(fbPtr->payloadBitErrors)))
    {
        ce->errorFreePayloads++;
        if (verbose) printf("Error Free payload!\n");
    }

    ce->PER = ((float)ce->frameNumber-(float)ce->errorFreePayloads)/((float)ce->frameNumber);
    ce->lastReceivedFrame = fbPtr->iteration;
    ce->BERLastPacket = ((float)fbPtr->payloadBitErrors)/((float)(ce->payloadLen*8));
    ce->weightedAvg += (float) fbPtr->payload_valid;

    //printf("ce->goal=%s\n", ce->goal);

    // Update goal value
    if (strcmp(ce->goal, "payload_valid") == 0)
    {
        if (verbose) printf("Goal is payload_valid. Setting latestGoalValue to %d\n", fbPtr->payload_valid);
        ce->latestGoalValue = fbPtr->payload_valid;
    }
    else if (strcmp(ce->goal, "X_valid_payloads") == 0)
    {
        if (verbose) printf("Goal is X_valid_payloads. Setting latestGoalValue to %u\n", ce->validPayloads);
        ce->latestGoalValue = (float) ce->validPayloads;
    }
    else if (strcmp(ce->goal, "X_errorFreePayloads") == 0)
    {
        if (verbose) printf("Goal is X_errorFreePayloads. Setting latestGoalValue to %u\n", ce->errorFreePayloads);
        ce->latestGoalValue = (float) ce->errorFreePayloads;
    }
    else if (strcmp(ce->goal, "X_frames") == 0)
    {
        if (verbose) printf("Goal is X_frames. Setting latestGoalValue to %u\n", ce->frameNumber);
        ce->latestGoalValue = (float) ce->frameNumber;
    }
    else if (strcmp(ce->goal, "X_seconds") == 0)
    {
if (verbose) printf("Goal is X_seconds. Setting latestGoalValue to %f\n", ce->runningTime);
        ce->latestGoalValue = ce->runningTime;
    }
    else
    {
        fprintf(stderr, "ERROR: Unknown Goal!\n");
        exit(EXIT_FAILURE);
    }
    // TODO: implement if statements for other possible goals

    return 1;
} // End ceProcessData()



int ceOptimized(struct CognitiveEngine * ce, int verbose)
{
	// Update running average
	ce->averagedGoalValue -= ce->metric_mem[ce->iteration%ce->averaging]/ce->averaging;
	ce->averagedGoalValue += ce->latestGoalValue/ce->averaging;
	ce->metric_mem[ce->iteration%ce->averaging] = ce->latestGoalValue;

	//printf("\nLatest: %.2f Average: %.2f Memory: %.2f\n\n", ce->latestGoalValue, ce->averagedGoalValue, ce->metric_mem[ce->iteration%ce->averaging]);
	
   	if(ce->frameNumber>ce->averaging){
		if (verbose) 
	   	{
		   printf("Checking if goal value has been reached.\n");
		   printf("ce.averagedGoalValue= %f\n", ce->averagedGoalValue);
		   printf("ce.threshold= %f\n", ce->threshold);
	   	}
	   	if (ce->latestGoalValue >= ce->threshold)
	   	{
		   if (verbose) printf("Goal is reached!\n");
		   return 1;
	   	}
	   	if (verbose) printf("Goal not reached yet.\n");
	}
   	return 0;
} // end ceOptimized()

int ceModifyTxParams(struct CognitiveEngine * ce, struct feedbackStruct * fbPtr, int verbose)
{
    int modify = 0;


    if (verbose) printf("ce->adaptationCondition= %s\n", ce->adaptationCondition);

    // Add 'user input' adaptation
    if(strcmp(ce->adaptationCondition, "user_specified") == 0) {
        // Check if parameters should be modified
        modify = 1;
        if (verbose) printf("user specified adaptation mode. Modifying...\n");
    }

    // Check what values determine if parameters should be modified
    if(strcmp(ce->adaptationCondition, "last_payload_invalid") == 0) {
        // Check if parameters should be modified
        if(fbPtr->payload_valid<1)
        {
            modify = 1;
            if (verbose) printf("lpi. Modifying...\n");
        }
    }
    if(strcmp(ce->adaptationCondition, "weighted_avg_payload_valid<X") == 0) {
        // Check if parameters should be modified
        if (ce->weightedAvg < ce->weighted_avg_payload_valid_threshold)
        {
            modify = 1;
            if (verbose) printf("wapv<X. Modifying...\n");
        }
    }
    if(strcmp(ce->adaptationCondition, "weighted_avg_payload_valid>X") == 0) {
        // Check if parameters should be modified
        if (ce->weightedAvg > ce->weighted_avg_payload_valid_threshold)
        {
            modify = 1;
            if (verbose) printf("wapv>X. Modifying...\n");
        }
    }
    if(strcmp(ce->adaptationCondition, "PER<X") == 0) {
        // Check if parameters should be modified
        if (verbose) printf("PER = %f\n", ce->PER);
        if(ce->PER < ce->PER_threshold)
        {
            modify = 1;
            if (verbose) printf("per<x. Modifying...\n" );
        }
    }
    if(strcmp(ce->adaptationCondition, "PER>X") == 0) {
        // Check if parameters should be modified
        if (verbose) printf("PER = %f\n", ce->PER);
        if(ce->PER > ce->PER_threshold)
        {
            modify = 1;
            if (verbose) printf("per>x. Modifying...\n" );
        }
    }
    if(strcmp(ce->adaptationCondition, "BER_lastPacket<X") == 0) {
        // Check if parameters should be modified
        if (verbose) printf("BER = %f\n", ce->BERLastPacket);
        if(ce->BERLastPacket < ce->BER_threshold)
        {
            modify = 1;
            if (verbose) printf("Ber_lastpacket<x. Modifying...\n" );
        }
    }
    if(strcmp(ce->adaptationCondition, "BER_lastPacket>X") == 0) {
        // Check if parameters should be modified
        if (verbose) printf("BER = %f\n", ce->BERLastPacket);
        if(ce->BERLastPacket > ce->BER_threshold)
        {
            modify = 1;
            if (verbose) printf("Ber_lastpacket>x. Modifying...\n" );
        }
    }
    if(strcmp(ce->adaptationCondition, "last_packet_error_free") == 0) {
        // Check if parameters should be modified
        if(!(fbPtr->payloadBitErrors)){
            modify = 1;
            if (verbose) printf("lpef. Modifying...\n");
        }
    }

    // If so, modify the specified parameter
    if (modify) 
    {
        if (verbose) printf("Modifying Tx parameters...\n");
        // TODO: Implement a similar if statement for each possible option
        // that can be adapted.

        // This can't work because rx can't detect signals with different 
        // number of subcarriers
        //if (strcmp(ce->adaptation, "decrease_numSubcarriers") == 0) {
        //    if (ce->numSubcarriers > 2)
        //        ce->numSubcarriers -= 2;
        //}

        if(strcmp(ce->adaptationCondition, "user_specified") == 0) {
            // Check if parameters should be modified
            if (verbose) printf("Reading user specified adaptations from user ce file: 'userEngine.txt'\n");
            readCEConfigFile(ce, (char*) "userEngine.txt", verbose);
        }

        if (strcmp(ce->adaptation, "increase_payload_len") == 0) {
            if (ce->payloadLen + ce->payloadLenIncrement <= ce->payloadLenMax) 
            {
                ce->payloadLen += ce->payloadLenIncrement;
            }
        }

        if (strcmp(ce->adaptation, "decrease_payload_len") == 0) {
            if (ce->payloadLen - ce->payloadLenIncrement >= ce->payloadLenMin) 
            {
                ce->payloadLen -= ce->payloadLenIncrement;
            }
        }

        if (strcmp(ce->adaptation, "decrease_mod_scheme_PSK") == 0) {
            if (strcmp(ce->modScheme, "QPSK") == 0) {
                strcpy(ce->modScheme, "BPSK");
            }
            if (strcmp(ce->modScheme, "8PSK") == 0) {
                strcpy(ce->modScheme, "QPSK");
            }
            if (strcmp(ce->modScheme, "16PSK") == 0) {
                strcpy(ce->modScheme, "8PSK");
            }
            if (strcmp(ce->modScheme, "32PSK") == 0) {
                strcpy(ce->modScheme, "16PSK");
            }
            if (strcmp(ce->modScheme, "64PSK") == 0) {
                strcpy(ce->modScheme, "32PSK");
            }
            if (strcmp(ce->modScheme, "128PSK") == 0) {
                strcpy(ce->modScheme, "64PSK");
                //printf("New modscheme: 64PSK\n");
            }
        }
        // Decrease ASK Modulations
        if (strcmp(ce->adaptation, "decrease_mod_scheme_ASK") == 0) {
            if (strcmp(ce->modScheme, "4ASK") == 0) {
                strcpy(ce->modScheme, "BASK");
            }
            if (strcmp(ce->modScheme, "8ASK") == 0) {
                strcpy(ce->modScheme, "4ASK");
            }
            if (strcmp(ce->modScheme, "16ASK") == 0) {
                strcpy(ce->modScheme, "8ASK");
            }
            if (strcmp(ce->modScheme, "32ASK") == 0) {
                strcpy(ce->modScheme, "16ASK");
	    }
            if (strcmp(ce->modScheme, "64ASK") == 0) {
                strcpy(ce->modScheme, "32ASK");
            }
            if (strcmp(ce->modScheme, "128ASK") == 0) {
                strcpy(ce->modScheme, "64ASK");
            }
        }
	// Turn outer FEC on/off
   	if (strcmp(ce->adaptation, "Outer FEC On/Off") == 0){
        if (verbose) printf("Adapt option: outer fec on/off. adapting...\n");
   	    // Turn FEC off
   	    if (ce->FECswitch == 1) {
   	        strcpy(ce->outerFEC_prev, ce->outerFEC);
   	        strcpy(ce->outerFEC, "none");
   	        ce->FECswitch = 0;
   	    }
   	    // Turn FEC on
   	    else {
   	        strcpy(ce->outerFEC, ce->outerFEC_prev);
   	        ce->FECswitch = 1;
   	    }
   	} 
        // Not use FEC
        if (strcmp(ce->adaptation, "no_fec") == 0) {
           if (strcmp(ce->outerFEC, "none") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "Hamming74") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "Hamming128") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "Golay2412") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "SEC-DED2216") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "SEC-DED3932") == 0) {
               strcpy(ce->outerFEC, "none");
           }
        }
        // FEC modifying (change to higher)
        if (strcmp(ce->adaptation, "increase_fec") == 0) {
           if (strcmp(ce->outerFEC, "SEC-DED3932") == 0) {
               strcpy(ce->outerFEC, "SEC-DED7264");
           } 
           if (strcmp(ce->outerFEC, "SEC-DED2216") == 0) {
               strcpy(ce->outerFEC, "SEC-DED3932");
           }
           if (strcmp(ce->outerFEC, "Golay2412") == 0) {
               strcpy(ce->outerFEC, "SEC-DED2216");
           }
           if (strcmp(ce->outerFEC, "Hamming128") == 0) {
               strcpy(ce->outerFEC, "Golay2412");
           }
           if (strcmp(ce->outerFEC, "Hamming74") == 0) {
               strcpy(ce->outerFEC, "Hamming128");
           }
           if (strcmp(ce->outerFEC, "none") == 0) {
               strcpy(ce->outerFEC, "Hamming74");
           }
        }
        // FEC modifying (change to lower)
        if (strcmp(ce->adaptation, "decrease_fec") == 0) {
           if (strcmp(ce->outerFEC, "Hamming74") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "Hamming128") == 0) {
               strcpy(ce->outerFEC, "Hamming74");
           }
           if (strcmp(ce->outerFEC, "Golay2412") == 0) {
               strcpy(ce->outerFEC, "Hamming128");
           }
           if (strcmp(ce->outerFEC, "SEC-DED2216") == 0) {
               strcpy(ce->outerFEC, "Golay2412");
           }
           if (strcmp(ce->outerFEC, "SEC-DED3932") == 0) {
               strcpy(ce->outerFEC, "SEC-DED2216");
           }
           if (strcmp(ce->outerFEC, "SEC-DED7264") == 0) {
               strcpy(ce->outerFEC, "SEC-DED3932");
           }
        }

        if (strcmp(ce->adaptation, "mod_scheme->BPSK") == 0) {
            strcpy(ce->modScheme, "BPSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->QPSK") == 0) {
            strcpy(ce->modScheme, "QPSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->8PSK") == 0) {
            strcpy(ce->modScheme, "8PSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->16PSK") == 0) {
            strcpy(ce->modScheme, "16PSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->328PSK") == 0) {
            strcpy(ce->modScheme, "32PSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->64PSK") == 0) {
            strcpy(ce->modScheme, "64PSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->8QAM") == 0) {
            strcpy(ce->modScheme, "8QAM");
        }
        if (strcmp(ce->adaptation, "mod_scheme->16QAM") == 0) {
            strcpy(ce->modScheme, "16QAM");
        }
        if (strcmp(ce->adaptation, "mod_scheme->32QAM") == 0) {
            strcpy(ce->modScheme, "32QAM");
        }
        if (strcmp(ce->adaptation, "mod_scheme->64QAM") == 0) {
            strcpy(ce->modScheme, "64QAM");
        }
        if (strcmp(ce->adaptation, "mod_scheme->OOK") == 0) {
            strcpy(ce->modScheme, "OOK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->4ASK") == 0) {
            strcpy(ce->modScheme, "4ASK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->8ASK") == 0) {
            strcpy(ce->modScheme, "8ASK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->16ASK") == 0) {
            strcpy(ce->modScheme, "16ASK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->32ASK") == 0) {
            strcpy(ce->modScheme, "32ASK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->64ASK") == 0) {
            strcpy(ce->modScheme, "64ASK");
        }
    }
    return 1;
}   // End ceModifyTxParams()

//uhd::usrp::multi_usrp::sptr initializeUSRPs()
//{
//    //uhd::device_addr_t hint; //an empty hint discovers all devices
//    //uhd::device_addrs_t dev_addrs = uhd::device::find(hint);
//    //std::string str = dev_addrs[0].to_string();
//    //const char * c = str.c_str();
//    //printf("First UHD Device found: %s\n", c ); 
//
//    //std::string str2 = dev_addrs[1].to_string();
//    //const char * c2 = str2.c_str();
//    //printf("Second UHD Device found: %s\n", c2 ); 
//
//    uhd::device_addr_t dev_addr;
//    // TODO: Allow setting of USRP Address from command line
//    dev_addr["addr0"] = "type=usrp1,serial=8b9cadb0";
//    //uhd::usrp::multi_usrp::sptr usrp= uhd::usrp::multi_usrp::make(dev_addr);
//    //dev_addr["addr0"] = "8b9cadb0";
//    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(dev_addr);
//
//    //Lock mboard clocks
//    //usrp->set_clock_source(ref);
//
//    // Set the TX freq (in Hz)
//    usrp->set_tx_freq(450e6);
//    printf("TX Freq set to %f MHz\n", (usrp->get_tx_freq()/1e6));
//    // Wait for USRP to settle at the frequency
//    while (not usrp->get_tx_sensor("lo_locked").to_bool()){
//        usleep(1000);
//        //sleep for a short time 
//    }
//    //printf("USRP tuned and ready.\n");
// 
//    // Set the rf gain (dB)
//    // TODO: Allow setting of gain from command line
//    usrp->set_tx_gain(0.0);
//    printf("TX Gain set to %f dB\n", usrp->get_tx_gain());
// 
//    // Set the rx_rate (Samples/s)
//    // TODO: Allow setting of tx_rate from command line
//    usrp->set_tx_rate(1e6);
//    printf("TX rate set to %f MS/s\n", (usrp->get_tx_rate()/1e6));
//
//    // Set the IF BW (in Hz)
//    usrp->set_tx_bandwidth(500e3);
//    printf("TX bandwidth set to %f kHz\n", (usrp->get_tx_bandwidth()/1e3));
//
//    return usrp;
//} // end initializeUSRPs()


int postTxTasks(struct CognitiveEngine * cePtr, struct feedbackStruct * fb_ptr, int verbose)
{
    // FIXME: Find another way to fix this:: FIXED?
    usleep(cePtr->delay_us);
	std::clock_t timeout_start = std::clock();
	while( !fb_ptr->block_flag ){
		std::clock_t timeout_now = std::clock();
		if(double(timeout_now-timeout_start)/CLOCKS_PER_SEC>1.0e-3) break;
	}
	fb_ptr->block_flag = 0;

    int DoneTransmitting = 0;

    // Process data from rx
    ceProcessData(cePtr, fb_ptr, verbose);
    // Modify transmission parameters (in fg and in USRP) accordingly
    if (!ceOptimized(cePtr, verbose)) 
    {
        if (verbose) printf("ceOptimized() returned false\n");
        ceModifyTxParams(cePtr, fb_ptr, verbose);
    }
    else
    {
        if (verbose) printf("ceOptimized() returned true\n");
        DoneTransmitting = 1;
        //printf("else: DoneTransmitting= %d\n", DoneTransmitting);
    }

    // For debugging
    if (verbose)
    {
        printf("in postTxTasks(): \nce.numSubcarriers= %u\n", cePtr->numSubcarriers);
        printf("ce.CPLen= %u\n", cePtr->CPLen);
    }

    return DoneTransmitting;
} // End postTxTasks()

void updateScenarioSummary(struct scenarioSummaryInfo *sc_sum, struct feedbackStruct *fb, struct CognitiveEngine *ce, int i_CE, int i_Sc){
	sc_sum->valid_headers[i_CE][i_Sc] += fb->header_valid;
	sc_sum->valid_payloads[i_CE][i_Sc] += fb->payload_valid;
	sc_sum->EVM[i_CE][i_Sc] += fb->evm;
	sc_sum->RSSI[i_CE][i_Sc] += fb->rssi;
	sc_sum->total_bits[i_CE][i_Sc] += ce->payloadLen;
	sc_sum->bit_errors[i_CE][i_Sc] += fb->payloadBitErrors;
}

void updateCognitiveEngineSummaryInfo(struct cognitiveEngineSummaryInfo *ce_sum, struct scenarioSummaryInfo *sc_sum, struct CognitiveEngine *ce, int i_CE, int i_Sc){
	// Decrement frameNumber once
	ce->frameNumber--;
	// Store metrics for scenario
	sc_sum->total_frames[i_CE][i_Sc] = ce->frameNumber;
	sc_sum->EVM[i_CE][i_Sc] /= ce->frameNumber;
	sc_sum->RSSI[i_CE][i_Sc] /= ce->frameNumber;
	sc_sum->PER[i_CE][i_Sc] = ce->PER;

	// Display the scenario summary
	printf("Cognitive Engine %i Scenario %i Summary:\nTotal frames: %i\nPercent valid headers: %2f\nPercent valid payloads: %2f\nAverage EVM: %2f\n"
		"Average RSSI: %2f\nAverage BER: %2f\nAverage PER: %2f\n\n", i_CE+1, i_Sc+1, sc_sum->total_frames[i_CE][i_Sc],
		(float)sc_sum->valid_headers[i_CE][i_Sc]/(float)sc_sum->total_frames[i_CE][i_Sc], (float)sc_sum->valid_payloads[i_CE][i_Sc]/(float)sc_sum->total_frames[i_CE][i_Sc],
		sc_sum->EVM[i_CE][i_Sc], sc_sum->RSSI[i_CE][i_Sc], (float)sc_sum->bit_errors[i_CE][i_Sc]/(float)sc_sum->total_bits[i_CE][i_Sc], sc_sum->PER[i_CE][i_Sc]);

	// Store the sum of scenario metrics for the cognitive engine
	ce_sum->total_frames[i_CE] += sc_sum->total_frames[i_CE][i_Sc];
	ce_sum->valid_headers[i_CE] += sc_sum->valid_headers[i_CE][i_Sc];
	ce_sum->valid_payloads[i_CE] += sc_sum->valid_payloads[i_CE][i_Sc];
	ce_sum->EVM[i_CE] += sc_sum->EVM[i_CE][i_Sc];
	ce_sum->RSSI[i_CE] += sc_sum->RSSI[i_CE][i_Sc];
	ce_sum->total_bits[i_CE] += sc_sum->total_bits[i_CE][i_Sc];
	ce_sum->bit_errors[i_CE] += sc_sum->bit_errors[i_CE][i_Sc];
	ce_sum->PER[i_CE] += sc_sum->PER[i_CE][i_Sc];
}

void terminate(int sig){
	exit(1);
}


//Struct that contains the variables for a node in an ad hoc broadcasting network
//The node has a cognitive engine, a list of the neighbor nodes it broadcasts to, and a
//list of the channel conditions for each of its neighbor nodes
struct Node{
	struct CognitiveEngine ce;
	int numberOfNeighbors;
	struct Node * neighborList[50];
	struct Scenario scenarioList[50];
	int done;
	msequence input;
	msequence output;
	int number;
};

//Struct that contains variables for a ad hoc broadcasting network of nodes
//Every node is contained in the nodeList and the transmitterList contains the nodes
//that are currently transmitting
struct Network{
	struct Node nodeList[50];
	struct Node * transmitterList[50];
	struct Node * nextList[50];
	char * adaption;
	char * adaptype;
	char * goal;
	int source;
	int transmitnumber;
	int nextnumber;
};

//Struct that contains the variables for the primary user
struct PU{
	int bursttime;
	int burstingtime;
	int resttime;
	int restingtime;
	int on;
	int transmitted;
	int corrupted;
	int change;
	char state;
	int time;
};

//Struct that contains the variables for the secondary user
struct SU{
	int bursttime;
	int burstingtime;
	int scantime;
	int scanningtime;
	int waittime;
	int waitingtime;
	int on;
	int transmitted;
	int corrupted;
	int change;
	char state;
	int time;
};


int dsaCallback(unsigned char *  _header,
               int              _header_valid,
               unsigned char *  _payload,
               unsigned int     _payload_len,
               int              _payload_valid,
               framesyncstats_s _stats,
               void *           _userdata)
{
    struct rxCBstruct * rxCBS_ptr = (struct rxCBstruct *) _userdata;
    int verbose = rxCBS_ptr->verbose;
	msequence rx_ms = *rxCBS_ptr->rx_ms_ptr; 
	int primary;
	int ones = 0;
	int zeroes = 0;
	int twos = 0;
	char received;

	//The different kinds of transmissions are determined by the content of the header
	//and its number of 0's, 1's, and 2's
	for(int i = 0; i<8; ++i){
		if(_header[i]==1){
			ones++;
		}
		if(_header[i]==0){
			zeroes++;
		}
		if(_header[i]==2){
			twos++;
		}
	}

	//If the message has all 1's or all 2's then a primary transmission was received
	//and the rxCBs struct changes its variables to show this information
	if(ones>zeroes || twos>zeroes){
		//primary = 1;
		rxCBS_ptr->primaryon = 1;
		received = 'f';
		//printf("\n\nPrimary transmission\n\n");
	}

	//If the message has all zeroes then it is a secondary transmission
	if(zeroes>ones && zeroes>twos){
		//primary = 0;
		//rxCBS_ptr->primaryon = 0;
		rxCBS_ptr->secondarysending = 1;
		received = 'F';
		//printf("\n\nSecondary transmission\n\n");
	}

    // Variables for checking number of errors 
    unsigned int payloadByteErrors  =   0;
    unsigned int payloadBitErrors   =   0;
    int j,m;
	unsigned int tx_byte;
	if(received == 'f') tx_byte = 1;
	if(received == 'F') tx_byte = 0;

    // Calculate byte error rate and bit error rate for payload
    for (m=0; m<(signed int)_payload_len; m++)
    {
		//tx_byte = msequence_generate_symbol(rx_ms,8);
		//printf( "%1i %1i\n", (signed int)_payload[m], tx_byte );
        if (((int)_payload[m] != tx_byte))
        {
            payloadByteErrors++;
            for (j=0; j<8; j++)
            {
				if ((_payload[m]&(1<<j)) != (tx_byte&(1<<j)))
                   payloadBitErrors++;
            }      
        }           
    }               
                    
    // Data that will be sent to server
    // TODO: Send other useful data through feedback array
	//printf("CALLBACK!!!\n\n");
    struct feedbackStruct fb = {};
    fb.header_valid         =   _header_valid;
    fb.payload_valid        =   _payload_valid;
    fb.payload_len          =   _payload_len;
    fb.payloadByteErrors    =   payloadByteErrors;
    fb.payloadBitErrors     =   payloadBitErrors;
    fb.evm                  =   _stats.evm;
    fb.rssi                 =   _stats.rssi;
    fb.cfo                  =   _stats.cfo;	
	//fb.ce_num				=	_header[0];
	//fb.sc_num				=	_header[1];
	fb.iteration			=	0;
	fb.block_flag			=	0;

	for(int i=0; i<4; i++)	fb.iteration += _header[i+2]<<(8*(3-i));

    if (false)
    {
        printf("In rxcallback():\n");
        printf("Header: %i %i %i %i %i %i %i %i\n", _header[0], _header[1], 
            _header[2], _header[3], _header[4], _header[5], _header[6], _header[7]);
        feedbackStruct_print(&fb);
    }

    // Receiver sends data to server*/
	//struct feedbackStruct fb = {};
	if(rxCBS_ptr->usrptype == 'p' or rxCBS_ptr->usrptype == 's'){
		struct message mess;
		mess.type = rxCBS_ptr->usrptype;
		mess.feed = fb;
		mess.purpose = received;
		mess.number = rxCBS_ptr->number;
		mess.client = rxCBS_ptr->client;
		++rxCBS_ptr->number;
	
		write(rxCBS_ptr->client, (void*)&mess, sizeof(mess));
	}

    return 0;

} // end rxCallback()




int main(int argc, char ** argv){
    // Seed the PRNG
    srand(time(NULL));

    // TEMPORARY VARIABLE
    int usingUSRPs = 0;
	int tester = 0;

    int verbose = 1;
    int verbose_explicit = 0;
    int dataToStdout = 0;    

    // For experiments with CR Networks.
    // Specifies whether this crts instance is managing the experiment.
    int isController = 0;

    unsigned int serverPort =1400;
    char * serverAddr = (char*) "127.0.0.1";

    // Frame Synchronizer parameters
    unsigned int numSubcarriers = 64;
    unsigned int CPLen = 16;
    unsigned int taperLen = 4;
    float bandwidth = 1.0e6;
    float frequency = 460.0e6;
    float uhd_rxgain = 20.0;
	int networking = 0;
	int dsa = 0;
	int broadcasting = 0;
	int secondary = 0;
	int primary = 0;
	int receiver = 0;

    // Check Program options
    int d;
    while ((d = getopt(argc,argv,"QRDStBPNuhqvdrsp:ca:f:b:G:M:C:T:")) != EOF) {
        switch (d) {
        case 'u':
        case 'h':   usage();                           return 0;
        case 'q':   verbose = 0;                            break;
        case 'v':   verbose = 1; verbose_explicit = 1;      break;
        case 'd':   dataToStdout = 1; 
                    if (!verbose_explicit) verbose = 0;     break;
        case 'r':   usingUSRPs = 1;                         break;
        case 's':   usingUSRPs = 0;                         break;
        case 'p':   serverPort = atoi(optarg);              break;
        case 'c':   isController = 1;                       break;
        case 'a':   serverAddr = optarg;                    break;
        case 'f':   frequency = atof(optarg);               break;
        case 'b':   bandwidth = atof(optarg);               break;
        case 'G':   uhd_rxgain = atof(optarg);              break;
        case 'M':   numSubcarriers = atoi(optarg);          break;
        case 'C':   CPLen = atoi(optarg);                   break;
        case 'T':   taperLen = atoi(optarg);                break;
		case 'N':   networking = 1; break;
		case 'D':   dsa = 1; break;
		case 'B':	broadcasting = 1; break;
		case 'Q':   tester = 1; break;
		//Designate the node as a primary user
		case 'P':	primary = 1; break;

		//Designate the node as a secondary user
		case 'S':	secondary = 1; break;

		//Designate the node as a receiver
		case 'R':	receiver = 1; break;
        //case 'p':   serverPort = atol(optarg);            break;
        //case 'f':   frequency = atof(optarg);           break;
        //case 'b':   bandwidth = atof(optarg);           break;
        //case 'G':   uhd_rxgain = atof(optarg);          break;
        //case 't':   num_seconds = atof(optarg);         break;
        default:
            verbose = 1;
        }   
    }   

    pthread_t TCPServerThread;   // Pointer to thread ID
    // Threading for using uhd_siggen (for when using USRPs)
    //pthread_t siggenThread;
    //int serverThreadReturn = 0;  // return value of creating TCPServer thread
    //pid_t uhd_siggen_pid;

    // Array that will be accessible to both Server and CE.
    // Server uses it to pass data to CE.
    struct feedbackStruct fb = {};
	fb.primaryon = 0;

    // For creating appropriate symbol length from 
    // number of subcarriers and CP Length
    unsigned int symbolLen;

    // Iterators
    int i_CE = 0;
    int i_Sc = 0;
    int DoneTransmitting = 0;
    int isLastSymbol = 0;
    char scenario_list [30][60];
    char cogengine_list [30][60];
    
    //printf("variables declared.\n");
	float floatnumber = 0.0;
    int NumCE=readCEMasterFile(cogengine_list, verbose);  
    int NumSc=readScMasterFile(scenario_list, verbose);  
    //printf ("\nCalled readScMasterFile function\n");

    // Cognitive engine struct used in each test
    struct CognitiveEngine ce = CreateCognitiveEngine();
    // Scenario struct used in each test
    struct Scenario sc = CreateScenario();

	//Message struct to pass info with TCP
	struct message msg;
	msg.msgreceived = 0;

    //printf("structs declared\n");
    // framegenerator object used in each test
    ofdmflexframegen fg;

    // framesynchronizer object used in each test
    ofdmflexframesync fs;

	// identical pseudo random sequence generators for tx and rx
	msequence tx_ms = msequence_create_default(9u);
	msequence rx_ms = msequence_create_default(9u);

    //printf("frame objects declared\n");

    // Buffers for packet/frame data
    unsigned char header[8];                       // Must always be 8 bytes for ofdmflexframe
    unsigned char payload[1000];                   // Large enough to accomodate any (reasonable) payload that
                                                   // the CE wants to use.

    // pointer for accessing header array when it has float values
    //unsigned int * header_u = (unsigned int *) header;

    std::complex<float> frameSamples[10000];      // Buffer of frame samples for each symbol.
                                                   // Large enough to accomodate any (reasonable) payload that 
                                                   // the CE wants to use.
    // USRP objects
    uhd::tx_metadata_t metaData;
    uhd::usrp::multi_usrp::sptr usrp;
    uhd::tx_streamer::sptr txStream;

	float throughput = 0;
	float total_symbols;
	float payload_symbols;

	// Metric Summary structs for each scenario and each cognitive engine
	struct scenarioSummaryInfo sc_sum;
	struct cognitiveEngineSummaryInfo ce_sum;
                                                   
    ////////////////////// End variable initializations.///////////////////////

	signal(SIGTERM, terminate);
	signal(SIGINT, terminate);
	signal(SIGQUIT, terminate);
	signal(SIGKILL, terminate);

    // Begin TCP Server Thread
    //serverThreadReturn = pthread_create( &TCPServerThread, NULL, startTCPServer, (void*) feedback);
	floatnumber = 0.0;
    struct serverThreadStruct ss = CreateServerStruct();
    ss.serverPort = serverPort;
    ss.fb_ptr = &fb;
	ss.floatnumber = &floatnumber;
	ss.m_ptr = &msg;
	if(dsa==1){
		printf("dsa\n");
		ss.type = 'd';
	}
	else{
		ss.type = 'n';
	}
    if (isController or (broadcasting == 1 and usingUSRPs == 1)) 
        pthread_create( &TCPServerThread, NULL, startTCPServer, (void*) &ss);

    struct rxCBstruct rxCBs = CreaterxCBStruct();
    rxCBs.bandwidth = bandwidth;
    rxCBs.serverPort = serverPort;
    rxCBs.serverAddr = serverAddr;
    rxCBs.verbose = verbose;
	rxCBs.rx_ms_ptr = &rx_ms;

	//Becomes a one if a primary transmission has been detected
	rxCBs.primaryon = 1;

	//Becomes a one if a secondary transmission has been received
	rxCBs.secondarysending = 0;

    // Allow server time to finish initialization
    usleep(0.1e6);

	//signal (SIGPIPE, SIG_IGN);
	int client;
	const int socket_to_server = socket(AF_INET, SOCK_STREAM, 0);
	if(!isController  || !usingUSRPs){
		// Create a client TCP socket] 
		if( socket_to_server < 0)
		{   
		    fprintf(stderr, "ERROR: Receiver Failed to Create Client Socket. \nerror: %s\n", strerror(errno));
		    exit(EXIT_FAILURE);
		}   
		//printf("Created client socket to server. socket_to_server: %d\n", socket_to_server);

		// Parameters for connecting to server
		struct sockaddr_in servAddr;
		memset(&servAddr, 0, sizeof(servAddr));
		servAddr.sin_family = AF_INET;
		servAddr.sin_port = htons(serverPort);
		servAddr.sin_addr.s_addr = inet_addr(serverAddr);

		// Attempt to connect client socket to server
		int connect_status;
		if((connect_status = connect(socket_to_server, (struct sockaddr*)&servAddr, sizeof(servAddr))))
		{   
		    fprintf(stderr, "Receiver Failed to Connect to server.\n");
		    fprintf(stderr, "connect_status = %d\n", connect_status);
		    exit(EXIT_FAILURE);
		}

		rxCBs.client = socket_to_server;
		client = rxCBs.client;
        if (verbose)
            printf("Connected to Server.\n");
	}
	if(usingUSRPs && isController or (broadcasting == 1 and usingUSRPs == 1)){
		printf("\nPress any key once all nodes have connected to the TCP server\n");
		getchar();
	}

    // Get current date and time

    char dataFilename[50];
    time_t now = time(NULL);
    struct tm *t  = localtime(&now);
    strftime(dataFilename, sizeof(dataFilename)-1, "data/data_crts_%d%b%Y_%T", t);
    // TODO: Make sure data folder exists
    
    // Initialize Data File
    FILE * dataFile;
    if (dataToStdout)
    {
        dataFile = stdout;
    }
    else
    {
        dataFile = fopen(dataFilename, "w");
    }

    // Begin running tests

    // For each Cognitive Engine

	if(dsa==0 && broadcasting==0 && networking == 0 && receiver == 0 && tester == 0){

    for (i_CE=0; i_CE<NumCE; i_CE++)
    {

		if (verbose) 
            printf("\nStarting Tests on Cognitive Engine %d\n", i_CE+1);
        
        // Run each CE through each scenario
        for (i_Sc= 0; i_Sc<NumSc; i_Sc++)
        {

            // Initialize current CE
            ce = CreateCognitiveEngine();
            if (isController)
            {
                readCEConfigFile(&ce,cogengine_list[i_CE], verbose);

                if (verbose) printf("\n\nStarting Scenario %d\n", i_Sc+1);
                // Initialize current Scenario
                sc = CreateScenario();
                readScConfigFile(&sc,scenario_list[i_Sc], verbose);

                fprintf(dataFile, "Cognitive Engine %d\nScenario %d\n", i_CE+1, i_Sc+1);
                //All metrics
                /*fprintf(dataFile, "%-10s %-10s %-14s %-15s %-10s %-10s %-10s %-19s %-16s %-18s \n",
                    "linetype","frameNum","header_valid","payload_valid","evm (dB)","rssi (dB)","PER","payloadByteErrors","BER:LastPacket","payloadBitErrors");*/
                //Useful metrics
                fprintf(dataFile, "%-10s %-10s %-10s %-10s %-8s %-12s %-12s %-20s %-19s\n",
                    "linetype","frameNum","evm (dB)","rssi (dB)","PER","Packet BER", "Throughput", "Spectral Efficiency", "Averaged Goal Value");
                fflush(dataFile);
            }
            else
            // If this crts instance is not the controller,
            // then no CEs, adaptations, scenarios, etc.
            {
                ce.numSubcarriers = numSubcarriers;
                ce.CPLen = CPLen;
                ce.taperLen = taperLen;
                ce.frequency = frequency;
                ce.bandwidth = bandwidth;
            }

            // Initialize Receiver Defaults for current CE and Sc
            ce.frameNumber = 1;
            fs = CreateFS(ce, sc, &rxCBs);

            std::clock_t begin = std::clock();
            std::clock_t now;
            // Begin Testing Scenario
            DoneTransmitting = 0;

            //while(!DoneTransmitting)
            //{
                if (usingUSRPs) 
                {
                    //usrp = initializeUSRPs();    
                    // create transceiver object
                    unsigned char * p = NULL;   // default subcarrier allocation
                    if (verbose) 
                        printf("Using ofdmtxrx\n");
                    ofdmtxrx txcvr(ce.numSubcarriers, ce.CPLen, ce.taperLen, p, rxCallback, (void*) &rxCBs);

                    // Start the Scenario simulations from the scenario USRPs
                    //enactUSRPScenario(ce, sc, &uhd_siggen_pid);

                    while(!DoneTransmitting)
                    {
                        // set properties
                        txcvr.set_tx_freq(ce.frequency);
                        txcvr.set_tx_rate(ce.bandwidth);
                        txcvr.set_tx_gain_soft(ce.txgain_dB);
                        txcvr.set_tx_gain_uhd(ce.uhd_txgain_dB);
                        //txcvr.set_tx_antenna("TX/RX");

                        if (!isController)
                        {
                            txcvr.set_rx_freq(frequency);
                            txcvr.set_rx_rate(bandwidth);
                            txcvr.set_rx_gain_uhd(uhd_rxgain);

                            if (verbose)
                            {
                                txcvr.debug_enable();
                                printf("Set Rx freq to %f\n", frequency);
                                printf("Set Rx rate to %f\n", bandwidth);
                                printf("Set uhd Rx gain to %f\n", uhd_rxgain);
                            }

                            int continue_running = 1;
							int rflag;
							char readbuffer;
							txcvr.start_rx();
                            while(continue_running)
                            {
								// Wait until server closes or there is an error, then exit
								rflag = recv(socket_to_server, &readbuffer, sizeof(readbuffer), 0);
								printf("Rx flag: %i\n", rflag);
								if(rflag == 0 || rflag == -1){
									close(socket_to_server);
									msequence_destroy(rx_ms);
									exit(1);
								}
                            }
                        }

                        if (verbose) {
                            txcvr.debug_enable();
                            printf("Set frequency to %f\n", ce.frequency);
                            printf("Set bandwidth to %f\n", ce.bandwidth);
                            printf("Set txgain_dB to %f\n", ce.txgain_dB);
                            printf("Set uhd_txgain_dB to %f\n", ce.uhd_txgain_dB);
                            printf("Set Tx antenna to %s\n", "TX/RX");
                        }

                        int i = 0;
                        // Generate data
                        if (verbose) printf("\n\nGenerating data that will go in frame...\n");
						header[0] = i_CE+1;
						header[1] = i_Sc+1;
                        for (i=0; i<4; i++)
                            header[i+2] = (ce.frameNumber & (0xFF<<(8*(3-i))))>>(8*(3-i));
						header[6] = 0;
						header[7] = 0;
                        for (i=0; i<(signed int)ce.payloadLen; i++)
                            payload[i] = (unsigned char)msequence_generate_symbol(tx_ms,8);

                        // Include frame number in header information
                        //* header_u = ce.frameNumber;
                        if (verbose) printf("Frame Num: %u\n", ce.frameNumber);

                        // Set Modulation Scheme
                        if (verbose) printf("Modulation scheme: %s\n", ce.modScheme);
                        modulation_scheme ms = convertModScheme(ce.modScheme, &ce.bitsPerSym);

                        // Set Cyclic Redundency Check Scheme
                        //crc_scheme check = convertCRCScheme(ce.crcScheme);

                        // Set inner forward error correction scheme
                        if (verbose) printf("Inner FEC: ");
                        fec_scheme fec0 = convertFECScheme(ce.innerFEC, verbose);

                        // Set outer forward error correction scheme
                        if (verbose) printf("Outer FEC: ");
                        fec_scheme fec1 = convertFECScheme(ce.outerFEC, verbose);

                        //txcvr.transmit_packet(header, payload, ce.payloadLen, ms, fec0, fec1);
                        // Replace with txcvr methods that allow access to samples:
                        txcvr.assemble_frame(header, payload, ce.payloadLen, ms, fec0, fec1);
                        int isLastSymbol = 0;
                        while(!isLastSymbol)
                        {
                            isLastSymbol = txcvr.write_symbol();
                            enactScenarioBaseband(txcvr.fgbuffer, ce, sc);
                            txcvr.transmit_symbol();
                        }
                        txcvr.end_transmit_frame();

                        DoneTransmitting = postTxTasks(&ce, &fb, verbose);
                        // Record the feedback data received
                        //TODO: include fb.cfo

						// Compute throughput and spectral efficiency
						payload_symbols = (float)ce.payloadLen/(float)ce.bitsPerSym;
						total_symbols = (float)ofdmflexframegen_getframelen(txcvr.fg);
						throughput = (float)ce.bitsPerSym*ce.bandwidth*(payload_symbols/total_symbols);

                        //All metrics
                        /*fprintf(dataFile, "%-10s %-10u %-14i %-15i %-10.2f %-10.2f %-8.2f %-19u %-12.2f %-16u %-12.2f %-20.2f %-19.2f\n", 
							"crtsdata:", fb.frameNum, fb.header_valid, fb.payload_valid, fb.evm, fb.rssi, ce.PER, fb.payloadByteErrors,
							ce.BERLastPacket, fb.payloadBitErrors, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);*/
						//Useful metrics
						fprintf(dataFile, "%-10s %-10i %-10.2f %-10.2f %-8.2f %-12.2f %-12.2f %-20.2f %-19.2f\n", 
							"crtsdata:", fb.iteration,  fb.evm, fb.rssi, ce.PER,
							ce.BERLastPacket, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);
                        fflush(dataFile);

                        // Increment the frame counter
                        ce.frameNumber++;
						ce.iteration++;

                        // Update the clock
                        now = std::clock();
                        ce.runningTime = double(now-begin)/CLOCKS_PER_SEC;

						updateScenarioSummary(&sc_sum, &fb, &ce, i_CE, i_Sc);
                    } // End If while loop
                }
                else // If not using USRPs
                {
                    while(!DoneTransmitting)
                    {
                        // Initialize Transmitter Defaults for current CE and Sc
                        fg = CreateFG(ce, sc, verbose);  // Create ofdmflexframegen object with given parameters
                        if (verbose) ofdmflexframegen_print(fg);

                        // Iterator
                        int i = 0;

                        // Generate data
                        if (verbose) printf("\n\nGenerating data that will go in frame...\n");
                        header[0] = i_CE+1;
						header[1] = i_Sc+1;
                        for (i=0; i<4; i++)
                            header[i+2] = (ce.frameNumber & (0xFF<<(8*(3-i))))>>(8*(3-i));
						header[6] = 0;
						header[7] = 0;
                        for (i=0; i<(signed int)ce.payloadLen; i++)
                            payload[i] = (unsigned char)msequence_generate_symbol(tx_ms,8);

						// Called just to update bits per symbol field
						convertModScheme(ce.modScheme, &ce.bitsPerSym);

                        // Assemble frame
                        ofdmflexframegen_assemble(fg, header, payload, ce.payloadLen);
                        //printf("DoneTransmitting= %d\n", DoneTransmitting);
						
                        // i.e. Need to transmit each symbol in frame.
                        isLastSymbol = 0;

                        while (!isLastSymbol) 
                        {
                            //isLastSymbol = txTransmitPacket(ce, &fg, frameSamples, metaData, txStream, usingUSRPs);
                            isLastSymbol = ofdmflexframegen_writesymbol(fg, frameSamples);
                            enactScenarioBaseband(frameSamples, ce, sc);
							
                            // Rx Receives packet
                            symbolLen = ce.numSubcarriers + ce.CPLen;
							ofdmflexframesync_execute(fs, frameSamples, symbolLen);
							
                        } // End Transmition For loop
			
                        DoneTransmitting = postTxTasks(&ce, &fb, verbose);

						fflush(dataFile);

						// Compute throughput and spectral efficiency
						payload_symbols = (float)ce.payloadLen/(float)ce.bitsPerSym;
						total_symbols = (float)ofdmflexframegen_getframelen(fg);
						throughput = (float)ce.bitsPerSym*ce.bandwidth*(payload_symbols/total_symbols);

                        //All metrics
                        /*fprintf(dataFile, "%-10s %-10u %-14i %-15i %-10.2f %-10.2f %-8.2f %-19u %-12.2f %-16u %-12.2f %-20.2f %-19.2f\n", 
							"crtsdata:", fb.frameNum, fb.header_valid, fb.payload_valid, fb.evm, fb.rssi, ce.PER, fb.payloadByteErrors,
							ce.BERLastPacket, fb.payloadBitErrors, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);*/
						//Useful metrics
						fprintf(dataFile, "%-10s %-10i %-10.2f %-10.2f %-8.2f %-12.2f %-12.2f %-20.2f %-19.2f\n", 
							"crtsdata:", fb.iteration,  fb.evm, fb.rssi, ce.PER,
							ce.BERLastPacket, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);

                        // Increment the frame counters and iteration counter
                        ce.frameNumber++;
						ce.iteration++;
                        // Update the clock
                        now = std::clock();
                        ce.runningTime = double(now-begin)/CLOCKS_PER_SEC;

						updateScenarioSummary(&sc_sum, &fb, &ce, i_CE, i_Sc);
                    } // End else While loop					
                }


            //} // End Test While loop
            clock_t end = clock();
            double time = (end-begin)/(double)CLOCKS_PER_SEC + ce.iteration*ce.delay_us/1.0e6;
            //fprintf(dataFile, "Elapsed Time: %f (s)", time);
			fprintf(dataFile, "Begin: %li End: %li Clock/s: %li Time: %f", begin, end, CLOCKS_PER_SEC, time);
            fflush(dataFile);

            // Reset the goal
            ce.latestGoalValue = 0.0;
            ce.errorFreePayloads = 0;
            if (verbose) printf("Scenario %i completed for CE %i.\n", i_Sc+1, i_CE+1);
            fprintf(dataFile, "\n\n");
            fflush(dataFile);

			updateCognitiveEngineSummaryInfo(&ce_sum, &sc_sum, &ce, i_CE, i_Sc);

			// Reset frame number
			ce.frameNumber = 0;
            
        } // End Scenario For loop

        if (verbose) printf("Tests on Cognitive Engine %i completed.\n", i_CE+1);

		// Divide the sum of each metric by the number of scenarios run to get the final metric
		ce_sum.EVM[i_CE] /= i_Sc;
		ce_sum.RSSI[i_CE] /= i_Sc;
		ce_sum.PER[i_CE] /= i_Sc;

		// Print cognitive engine summaries
		printf("Cognitive Engine %i Summary:\nTotal frames: %i\nPercent valid headers: %2f\nPercent valid payloads: %2f\nAverage EVM: %2f\n"
			"Average RSSI: %2f\nAverage BER: %2f\nAverage PER: %2f\n\n", i_CE+1, ce_sum.total_frames[i_CE], (float)ce_sum.valid_headers[i_CE]/(float)ce_sum.total_frames[i_CE],
			(float)ce_sum.valid_payloads[i_CE]/(float)ce_sum.total_frames[i_CE], ce_sum.EVM[i_CE], ce_sum.RSSI[i_CE], (float)ce_sum.bit_errors[i_CE]/(float)ce_sum.total_bits[i_CE], ce_sum.PER[i_CE]);

    } // End CE for loop

	// destroy objects
	msequence_destroy(tx_ms);
	msequence_destroy(rx_ms);
	close(socket_to_server);

	if(!usingUSRPs) close(socket_to_server);

    return 0;
}

//Simple Broadcasting
//Simulate broadcasting with one transmitting radio
if(broadcasting == 1 && usingUSRPs == 0){

printf("broadcasting\n");
printf("%d\n", usingUSRPs);
if(usingUSRPs && isController){
	printf("Hi\n");

}
if(usingUSRPs && !isController){
	printf("Hi\n");

}
//mainfb collects feedback from each receiver being broadcasted to and averages them together
	struct feedbackStruct mainfb;
	int fbnum = 0;

    mainfb.header_valid = 0;
    mainfb.payload_valid = 0;
    mainfb.payload_len = 0;
    mainfb.payloadByteErrors = 0;
    mainfb.payloadBitErrors = 0;
    mainfb.iteration = 0;
    mainfb.evm = 0;
    mainfb.rssi = 0;
    mainfb.cfo = 0;


	//configuration variables set up to read broadcast file to form broadcast structure
	config_t cfg; // Returns all parameters in this structure
    config_setting_t *setting;
	config_setting_t *iterator;
    const char * str; // Stores the value of the String Parameters in Config file
    int tmpI; // Stores the value of Integer Parameters from Config file
    double tmpD;
	char * str2;
    int numberOfRadios;
	struct Scenario scList[50];
	struct CognitiveEngine ceList[50];
	
    ce.frameNumber = 1;
    //fs = CreateFS(ce, sc, &rxCBs);
    if (verbose)
        printf("Reading %s\n", "master_broadcast_file.txt");

    //Initialization
    config_init(&cfg);

    // Read the file. If there is an error, report it and exit.
    if (!config_read_file(&cfg,"master_broadcast_file.txt"))
    {
        fprintf(stderr, "\n%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    };
    // Read the parameter group
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        // Read the strings
        if (config_setting_lookup_int(setting, "numberOfRadios", &tmpI))
        {
			numberOfRadios = tmpI;
			printf("%d\n", tmpI);
        };
        if (config_setting_lookup_string(setting, "ce", &str))
        {
			str2 = (char *)str;
			//for(int r = 0; r<numberOfRadios; ++r){
				ce = CreateCognitiveEngine();
				readCEConfigFile(&ce, str2, verbose);
			//};
        };
		for(int r=0; r<numberOfRadios; ++r){
			str = config_setting_get_string_elem(config_setting_get_member(setting, "scenarios"), r);
			str2 = (char *)str;
			scList[r] = CreateScenario();
			readScConfigFile(&scList[r], str2, verbose);
		};

    };
    config_destroy(&cfg);

	int i;
				//If USRPs are used one transceiver is initialized to transmit while all
				//othesr just receive and give feedback
                if (usingUSRPs) 
                {
                    //usrp = initializeUSRPs();    
                    // create transceiver object
                    unsigned char * p = NULL;   // default subcarrier allocation
                    if (verbose) 
                        printf("Using ofdmtxrx\n");
						printf("%d %d %d\n", ce.numSubcarriers, ce.CPLen, ce.taperLen);
                    ofdmtxrx txcvr(ce.numSubcarriers, ce.CPLen, ce.taperLen, p, rxCallback, (void*) &rxCBs);
					
                    // Start the Scenario simulations from the scenario USRPs
                    //enactUSRPScenario(ce, sc, &uhd_siggen_pid);

                    while(!DoneTransmitting)
                    {
                        // set properties
                        txcvr.set_tx_freq(ce.frequency);
                        txcvr.set_tx_rate(ce.bandwidth);
                        txcvr.set_tx_gain_soft(ce.txgain_dB);
                        txcvr.set_tx_gain_uhd(ce.uhd_txgain_dB);
                        //txcvr.set_tx_antenna("TX/RX");

                        if (!isController)
                        {
                            txcvr.set_rx_freq(frequency);
                            txcvr.set_rx_rate(bandwidth);
                            txcvr.set_rx_gain_uhd(uhd_rxgain);

                            if (verbose)
                            {
                                txcvr.debug_enable();
                                printf("Set Rx freq to %f\n", frequency);
                                printf("Set Rx rate to %f\n", bandwidth);
                                printf("Set uhd Rx gain to %f\n", uhd_rxgain);
                            }

                            int continue_running = 1;
							int rflag;
							char readbuffer;
							txcvr.start_rx();
                            while(continue_running)
                            {
								// Wait until server closes or there is an error, then exit
								
								rflag = recv(socket_to_server, &readbuffer, sizeof(readbuffer), 0);
								printf("Rx flag: %i\n", rflag);
								if(rflag == 0 || rflag == -1){
									close(socket_to_server);
									msequence_destroy(rx_ms);
									exit(1);
								}
                            }
                        }

                        if (verbose) {
                            txcvr.debug_enable();
                            printf("Set frequency to %f\n", ce.frequency);
                            printf("Set bandwidth to %f\n", ce.bandwidth);
                            printf("Set txgain_dB to %f\n", ce.txgain_dB);
                            printf("Set uhd_txgain_dB to %f\n", ce.uhd_txgain_dB);
                            printf("Set Tx antenna to %s\n", "TX/RX");
                        }

                        int i = 0;
                        // Generate data
                        if (verbose) printf("\n\nGenerating data that will go in frame...\n");
						header[0] = 1;
						header[1] = 1;
                        for (i=0; i<4; i++)
                            header[i+2] = (ce.frameNumber & (0xFF<<(8*(3-i))))>>(8*(3-i));
						header[6] = 0;
						header[7] = 0;
                        for (i=0; i<(signed int)ce.payloadLen; i++)
                            payload[i] = (unsigned char)msequence_generate_symbol(tx_ms,8);

                        // Include frame number in header information
                        //* header_u = ce.frameNumber;
                        if (verbose) printf("Frame Num: %u\n", ce.frameNumber);

                        // Set Modulation Scheme
                        if (verbose) printf("Modulation scheme: %s\n", ce.modScheme);
                        modulation_scheme ms = convertModScheme(ce.modScheme, &ce.bitsPerSym);

                        // Set Cyclic Redundency Check Scheme
                        //crc_scheme check = convertCRCScheme(ce.crcScheme);

                        // Set inner forward error correction scheme
                        if (verbose) printf("Inner FEC: ");
                        fec_scheme fec0 = convertFECScheme(ce.innerFEC, verbose);

                        // Set outer forward error correction scheme
                        if (verbose) printf("Outer FEC: ");
                        fec_scheme fec1 = convertFECScheme(ce.outerFEC, verbose);

                        //txcvr.transmit_packet(header, payload, ce.payloadLen, ms, fec0, fec1);
                        // Replace with txcvr methods that allow access to samples:
                        txcvr.assemble_frame(header, payload, ce.payloadLen, ms, fec0, fec1);
                        int isLastSymbol = 0;
                        while(!isLastSymbol)
                        {
							
                            isLastSymbol = txcvr.write_symbol();
                            //enactScenarioBaseband(txcvr.fgbuffer, ce, sc);
                            txcvr.transmit_symbol();
                        }
                        txcvr.end_transmit_frame();

                        DoneTransmitting = postTxTasks(&ce, &fb, verbose);
                        // Record the feedback data received
                        //TODO: include fb.cfo

						// Compute throughput and spectral efficiency
						payload_symbols = (float)ce.payloadLen/(float)ce.bitsPerSym;
						total_symbols = (float)ofdmflexframegen_getframelen(txcvr.fg);
						throughput = (float)ce.bitsPerSym*ce.bandwidth*(payload_symbols/total_symbols);

                        //All metrics
                        /*fprintf(dataFile, "%-10s %-10u %-14i %-15i %-10.2f %-10.2f %-8.2f %-19u %-12.2f %-16u %-12.2f %-20.2f %-19.2f\n", 
							"crtsdata:", fb.frameNum, fb.header_valid, fb.payload_valid, fb.evm, fb.rssi, ce.PER, fb.payloadByteErrors,
							ce.BERLastPacket, fb.payloadBitErrors, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);*/
						//Useful metrics
						/*fprintf(dataFile, "%-10s %-10i %-10.2f %-10.2f %-8.2f %-12.2f %-12.2f %-20.2f %-19.2f\n", 
							"crtsdata:", fb.iteration,  fb.evm, fb.rssi, ce.PER,
							ce.BERLastPacket, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);
                        fflush(dataFile);

                        // Increment the frame counter
                        ce.frameNumber++;
						ce.iteration++;

                        // Update the clock
                        now = std::clock();
                        ce.runningTime = double(now-begin)/CLOCKS_PER_SEC;

						updateScenarioSummary(&sc_sum, &fb, &ce, i_CE, i_Sc);*/
                    } // End If while loop
                }

	if(!usingUSRPs){
	std::clock_t begin = std::clock();
	std::clock_t now;
	for(int l = 0; l<1; ++l){
		mainfb.header_valid = 0;
		mainfb.payload_valid = 0;
		mainfb.payload_len = 0;
		mainfb.payloadByteErrors = 0;
		mainfb.payloadBitErrors = 0;
		mainfb.iteration = 0;
		mainfb.evm = 0;
		mainfb.rssi = 0;
		mainfb.cfo = 0;
		fbnum = 0;
		printf("\nTransmission %d", l + 1);
        if (verbose) printf("\n\nGenerating data that will go in frame...\n");
        header[0] = 1;
		header[1] = l+1;
        for (i=0; i<4; i++)
            header[i+2] = (ce.frameNumber & (0xFF<<(8*(3-i))))>>(8*(3-i));
		header[6] = 0;
		header[7] = 0;
        for (i=0; i<(signed int)ce.payloadLen; i++)
            payload[i] = (unsigned char)msequence_generate_symbol(tx_ms,8);
		for(int r = 0; r<numberOfRadios; ++r){
			sc = scList[r];
			msequence_reset(tx_ms);
			msequence_reset(rx_ms);
			for(int y = 0; y<l; ++y){
				msequence_advance(tx_ms);
				msequence_advance(rx_ms);
			};
			printf("The source radio broadcasted to radio %d\n", r + 1);
			fg = CreateFG(ce, sc, verbose);  // Create ofdmflexframegen object with given parameters
		    if (verbose) ofdmflexframegen_print(fg);

		    // Iterator
		    int i = 0;

			// Called just to update bits per symbol field
			convertModScheme(ce.modScheme, &ce.bitsPerSym);

		    // Assemble frame
		    ofdmflexframegen_assemble(fg, header, payload, ce.payloadLen);
		    //printf("DoneTransmitting= %d\n", DoneTransmitting);
		
		    // i.e. Need to transmit each symbol in frame.
		    isLastSymbol = 0;
			fbnum = 0;
		    while (!isLastSymbol) 
		    {
		        //isLastSymbol = txTransmitPacket(ce, &fg, frameSamples, metaData, txStream, usingUSRPs);
		        isLastSymbol = ofdmflexframegen_writesymbol(fg, frameSamples);
		        enactScenarioBaseband(frameSamples, ce, sc);
			
		        // Rx Receives packet
		        symbolLen = ce.numSubcarriers + ce.CPLen;
				ofdmflexframesync_execute(fs, frameSamples, symbolLen);
			//mainfb collects feedback data before averaging it
			mainfb.header_valid += fb.header_valid;
			mainfb.payload_valid += fb.payload_valid;
			mainfb.payload_len += fb.payload_len;
			mainfb.payloadByteErrors += fb.payloadByteErrors;
			mainfb.payloadBitErrors += fb.payloadBitErrors;
			mainfb.iteration += fb.iteration;
			mainfb.evm += fb.evm;
			mainfb.rssi += fb.rssi;
			mainfb.cfo += fb.cfo;
			
			//Accumulator to divide some feedback values by to average them
			++fbnum;



				
			}
			printf("%d\n", fbnum);
			//mainfb.header_valid /= fbnum;
			//mainfb.payload_valid /= fbnum;
			mainfb.payload_len /= fbnum;
			//mainfb.payloadByteErrors /= fbnum;
			//mainfb.payloadBitErrors /= fbnum;
			mainfb.iteration /= fbnum;
			mainfb.evm /= fbnum;
			mainfb.rssi /= fbnum;
			mainfb.cfo /= fbnum;
			//printf("\n\nmainfb\n\n");
			//feedbackStruct_print(&mainfb);
			}
			
			mainfb.header_valid /= numberOfRadios;
			mainfb.payload_valid /= numberOfRadios;
			mainfb.payload_len /= numberOfRadios;
			mainfb.payloadByteErrors /= numberOfRadios;
			mainfb.payloadBitErrors /= numberOfRadios;
			mainfb.iteration /= numberOfRadios;
			mainfb.evm /= numberOfRadios;
			mainfb.rssi /= numberOfRadios;
			mainfb.cfo /= numberOfRadios;
			printf("\n\nmainfb\n\n");
			feedbackStruct_print(&mainfb);
			

			//feedbackStruct_print(&mainfb);
			DoneTransmitting = postTxTasks(&ce, &mainfb, verbose);

			fflush(dataFile);

			// Compute throughput and spectral efficiency
			payload_symbols = (float)ce.payloadLen/(float)ce.bitsPerSym;
			total_symbols = (float)ofdmflexframegen_getframelen(fg);
			throughput = (float)ce.bitsPerSym*ce.bandwidth*(payload_symbols/total_symbols);

            //All metrics
            /*fprintf(dataFile, "%-10s %-10u %-14i %-15i %-10.2f %-10.2f %-8.2f %-19u %-12.2f %-16u %-12.2f %-20.2f %-19.2f\n", 
				"crtsdata:", fb.frameNum, fb.header_valid, fb.payload_valid, fb.evm, fb.rssi, ce.PER, fb.payloadByteErrors,
				ce.BERLastPacket, fb.payloadBitErrors, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);*/
			//Useful metrics
			fprintf(dataFile, "%-10s %-10i %-10.2f %-10.2f %-8.2f %-12.2f %-12.2f %-20.2f %-19.2f\n", 
				"crtsdata:", fb.iteration,  fb.evm, fb.rssi, ce.PER,
				ce.BERLastPacket, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);

            // Increment the frame counters and iteration counter
            ce.frameNumber++;
			ce.iteration++;
            // Update the clock
            now = std::clock();
            ce.runningTime = double(now-begin)/CLOCKS_PER_SEC;
		}}

	
	



	return 0;
}
//If networking and not using USRPs the master network file is read to make an ad hoc
//network for simulation
if(networking==1 && !usingUSRPs){

	verbose = 1;
    config_t cfg; // Returns all parameters in this structure
    config_setting_t *setting;
	config_setting_t *iterator;
    const char * str; // Stores the value of the String Parameters in Config file
    int tmpI; // Stores the value of Integer Parameters from Config file
    double tmpD;
	char * str2;
    int numberOfRadios;
	char radioname[10];

    if (verbose)
        printf("Reading %s\n", "master_network_file.txt");

    //Initialization
    config_init(&cfg);

    // Read the file. If there is an error, report it and exit.
    if (!config_read_file(&cfg,"master_network_file.txt"))
    {
        fprintf(stderr, "\n%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    }
	struct Network network;
    // Read the parameter group
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        // Read the strings
        if (config_setting_lookup_int(setting, "numberOfRadios", &tmpI))
        {
		numberOfRadios = tmpI;
        }

		//The radio doing the initial transmitting
        if (config_setting_lookup_int(setting, "source", &tmpI))
        {
		network.source = tmpI;
        }
    }
	
	network.transmitnumber = 1;
	//For each radio it reads its entry in the master file and makes a node struct with
	//the proper ce and neighbors to transmit to
    for(int i=0; i<numberOfRadios;++i){
		sprintf(radioname, "radio%d", i+1);
        printf("%s\n\n", radioname);
		network.nodeList[i].done = 0;
		network.nodeList[i].number = i;
		setting = config_lookup(&cfg, radioname);
		if (setting != NULL){
			if (config_setting_lookup_string(setting, "ce", &str)){
				network.nodeList[i].ce = CreateCognitiveEngine();
				str2=(char *)str;
				readCEConfigFile(&network.nodeList[i].ce, str2, verbose);
                printf("\n");
			};
			if (config_setting_lookup_int(setting, "numberOfNeighbors", &tmpI)){
				network.nodeList[i].numberOfNeighbors = tmpI;
                printf("Number of Neighbors: %d\n\n", tmpI);
			};
			for(int j=0; j<network.nodeList[i].numberOfNeighbors; ++j){
				tmpI = config_setting_get_int_elem(config_setting_get_member(setting, "neighbors"), j);
				network.nodeList[i].neighborList[j] = &network.nodeList[tmpI-1];
				str = config_setting_get_string_elem(config_setting_get_member(setting, "scenarios"), j);
				str2 = (char *)str;
				network.nodeList[i].scenarioList[j] = CreateScenario();
				readScConfigFile(&network.nodeList[i].scenarioList[j], str2, verbose);
                printf("\n");
			};
				
		};
	};

	//The transmitter list contains all nodes that will transmit next. At first it will only
	//contain the source node. Then it will contain all nodes that received the source's
	//broadcast that will be transmitting to other nodes, etc
	network.transmitterList[0] = &network.nodeList[network.source-1];
    config_destroy(&cfg);

	//Pointer to the currently transmitting node
	struct Node * nodePtr;
    while(network.transmitnumber>0){
		//The number of nodes that will be transmitting in the next round
		network.nextnumber = 0;
		for (int m=0; m<network.transmitnumber; m++){

		nodePtr = network.transmitterList[m];
		//Each transmitting node broadcasts to each of its neighbors
		//If the receiving node has neighbors it is added to the next list to show that it
		//will transmit in the next round
		for(int j=0; j<nodePtr->numberOfNeighbors; ++j){
	    	nodePtr->ce.frameNumber = 1;

			printf("\nNode %d broadcasting to Node %d\n\n", nodePtr->number + 1, nodePtr->neighborList[j]->number + 1);
			//If the receiving node has neighbors it is put in the next list
			if(nodePtr->neighborList[j]->numberOfNeighbors>0){
				network.nextList[network.nextnumber] = nodePtr->neighborList[j];
				network.nextnumber++;
				}
		    }
			
	    }
		//All nodes in the next list are moved to the transmit list before the loop restarts
		for(int g=0; g<network.nextnumber; ++g){
			network.transmitterList[g] = &network.nodeList[network.nextList[g]->number];

		};

		network.transmitnumber = network.nextnumber;
	}


return 0;
}

//If DSA is used but USRPs are not used then a simple DSA simulation is done
if(dsa==1 && !usingUSRPs){

	//Initialize variables for DSA and configuratin file reading
	struct PU pu;
	struct SU su;
	int wasted = 0;
	pu.transmitted = 0;
	su.transmitted = 0;
	pu.corrupted = 0;
	su.corrupted = 0;

	//Initially the primary user is on and transmitting while the secondary user is off
	//and scanning
	pu.on = 1;
	su.on = 0;
	pu.state = 't';
	su.state = 's';
	pu.change = 0;
	su.change = 0;
	pu.burstingtime = pu.bursttime;
	int totaltime = 100;
	verbose = 1;
    config_t cfg; // Returns all parameters in this structure
    config_setting_t *setting;
	config_setting_t *iterator;
    const char * str; // Stores the value of the String Parameters in Config file
    int tmpI; // Stores the value of Integer Parameters from Config file
    double tmpD;
	char * str2;

    if (verbose)
        printf("Reading %s\n", "master_dsa_file.txt");

    //Initialization
    config_init(&cfg);

    // Read the file. If there is an error, report it and exit.
    if (!config_read_file(&cfg,"master_dsa_file.txt"))
    {
        fprintf(stderr, "\n%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    }
    // Read the parameter group
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        // Read the strings
        if (config_setting_lookup_int(setting, "totaltime", &tmpI))
        {
		totaltime = tmpI;
        }
    }
    setting = config_lookup(&cfg, "PU");
    if (setting != NULL)
    {
        // Read the strings
        if (config_setting_lookup_int(setting, "bursttime", &tmpI))
        {
		pu.bursttime = tmpI;
        }
        if (config_setting_lookup_int(setting, "resttime", &tmpI))
        {
		pu.resttime = tmpI;
        }
    }
    setting = config_lookup(&cfg, "SU");
    if (setting != NULL)
    {
        // Read the strings
        if (config_setting_lookup_int(setting, "bursttime", &tmpI))
        {
		su.bursttime = tmpI;
        }
        if (config_setting_lookup_int(setting, "scantime", &tmpI))
        {
		su.scantime = tmpI;
        }
        if (config_setting_lookup_int(setting, "waittime", &tmpI))
        {
		su.waittime = tmpI;
        }
    }
	pu.burstingtime = pu.bursttime;
	pu.time = pu.bursttime;
	su.scanningtime = su.scantime;
	su.time = su.scantime;
	su.state = 's';
	//For loop iterates for the number of time units determined by the master dsa file
	for(int i = 0; i<totaltime; ++i){

		//If pu.time==0 then it is time for it to change its state
		//If it is on then it will turn off for its rest time
		//If it is off then it will transmit a burst for burst time
		if(pu.time == 0){
			if(pu.on == 1){
				pu.on = 0;
				pu.time = pu.resttime;
			}
			else{
				pu.on = 1;
				pu.time = pu.bursttime;
			}
		}
		//If the secondary user's time is at 0 then it must change its state
		//If it is scanning then it will start waiting
		//If it is done waiting then it will transmit
		//If it is done transmitting then it will scan
		while(su.time <= 0){
			if(su.state == 's' && su.time <= 0){
				su.state = 'w';
				su.time = su.waittime;
			}
			if(su.state == 'w' && su.time <= 0){
				su.state = 't';
				su.on = 1;
				su.time = su.bursttime;
			}
			if(su.state == 't' && su.time <= 0){
				su.state = 's';
				su.on = 0;
				su.time = su.scantime;
			}
		}

		//Prints out a message based on who is transmitting
		if(su.on==1 && pu.on==1){
			printf("Collison!!!\n");
			++pu.corrupted;
			++su.corrupted;
		};
		if(su.on==1 && pu.on==0){
			printf("The secondary user transmitted\n");
			++su.transmitted;
		};
		if(su.on==0 && pu.on==1){
			printf("The primary user transmitted\n");
			++pu.transmitted;

			//If the primary user transmits while the secondary user scans then the 
			//secondary user's scan time will reset
			if(su.state == 's'){
				su.time = su.scantime;
			}
		}
		if(su.on==0 && pu.on==0){
			printf("No one transmitted\n");
			++wasted;
		}

		//Decrement the time before the loop reiterates
		--pu.time;
		--su.time;
	}

	//After the loop has finished the number of successful and corrupted transmissions
	//are displayed
	printf("The primary user sent %d transmissions successfully\n", pu.transmitted);
	printf("The secondary user sent %d transmissions successfully\n", su.transmitted);
	printf("%d transmissions were corrupted\n", pu.corrupted);
	return 0;
}

//If DSA is used with USRPs and not a receiver either a primary transmitter is made or a
//secondary transmitter with sensing capabilities
if(dsa==1 && usingUSRPs && !receiver && !isController){

	pthread_t receiverfeedbackThread;
	struct message mess;
	int primarymsgnumber = 1;
	int secondarymsgnumber = 1;
	double primarybursttime;
	double primaryresttime;
	double secondarybursttime;
	double secondaryscantime;
	int primaryburstrandom;
	int primaryrestrandom;
	struct CognitiveEngine puce;
	struct CognitiveEngine suce;
	struct Scenario sc = CreateScenario();
	int totaltime = 100;
	verbose = 1;
	config_t cfg; // Returns all parameters in this structure
	config_setting_t *setting;
	config_setting_t *iterator;
	const char * str; // Stores the value of the String Parameters in Config file
	int tmpI; // Stores the value of Integer Parameters from Config file
	double tmpD;
	double tmpd;
	char * str2;
	//struct serveClientStruct * sc_ptr = (struct serveClientStruct*) _sc_ptr;
	int cl = rxCBs.client;


	if (verbose)
		printf("Reading %s\n", "master_dsa_file.txt");

	//Initialization
	config_init(&cfg);

	// Read the file. If there is an error, report it and exit.
	if (!config_read_file(&cfg,"master_dsa_file.txt"))
	{
		fprintf(stderr, "\n%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
		config_destroy(&cfg);
		exit(EX_NOINPUT);
	}
	// Read the parameter group
	setting = config_lookup(&cfg, "params");
	if (setting != NULL)
	{
		// Read the strings
		if (config_setting_lookup_int(setting, "totaltime", &tmpI))
		{
		totaltime = tmpI;
		}
	}
	setting = config_lookup(&cfg, "PU");
	if (setting != NULL)
	{
		// Read the strings
		if (config_setting_lookup_float(setting, "bursttime", &tmpD))
		{
		primarybursttime = tmpD;
		printf("%f\n", primarybursttime);
		}
		if (config_setting_lookup_float(setting, "resttime", &tmpD))
		{
		primaryresttime = tmpD;
		}
		if (config_setting_lookup_int(setting, "burstrandom", &tmpI))
		{
		primaryburstrandom = tmpI;
		}
		if (config_setting_lookup_int(setting, "restrandom", &tmpI))
		{
		primaryrestrandom = tmpI;
		}
		if (config_setting_lookup_string(setting, "ce", &str))
		{
		str2 = (char *)str;
		puce = CreateCognitiveEngine();
		readCEConfigFile(&puce, str2, verbose);
		}
	}
	setting = config_lookup(&cfg, "SU");
	if (setting != NULL)
	{
		// Read the strings
		if (config_setting_lookup_float(setting, "bursttime", &tmpD))
		{
		secondarybursttime = tmpD;
		}
		if (config_setting_lookup_float(setting, "scantime", &tmpD))
		{
		secondaryscantime = tmpD;
		}

		if (config_setting_lookup_string(setting, "ce", &str))
		{
		str2 = (char *)str;
		suce = CreateCognitiveEngine();
		readCEConfigFile(&suce, str2, verbose);
		}
	}
	
	//If it is a primary transmitter then the USRP ofdmtxrx object tranmists for its burst
	//time then rest for its rest time
	if(primary == 1){
		struct broadcastfeedbackinfo bfi;
		float timedivisor = 5.0;
		if(broadcasting==1){
		timedivisor = 1.0;
		bfi.client = rxCBs.client;
		bfi.m_ptr = &msg;
		bfi.msgnumber = &primarymsgnumber;
		pthread_create( &receiverfeedbackThread, NULL, feedbackThread, (void*) &bfi);
		}
		mess.type = 'P';
		rxCBs.usrptype = 'P';
		verbose = 0;
		float a;
		printf("primary\n");
		int h;

		//The primary user transmits a frame of all 1's for easy identification
		for(h = 0; h<8; h++){
			header[h] = 1;
		};
		for(h = 0; h<puce.payloadLen; h++){
			payload[h] = 1;
		};
		std::clock_t start;
		std::clock_t current;
		unsigned char * p = NULL;   // default subcarrier allocation
		if (verbose) 
		printf("Using ofdmtxrx\n");
		printf("%d %d %d\n", puce.numSubcarriers, puce.CPLen, puce.taperLen);
		ofdmtxrx txcvr(puce.numSubcarriers, puce.CPLen, puce.taperLen, p, rxCallback, (void*) &rxCBs);
		txcvr.set_tx_freq(puce.frequency);
		txcvr.set_tx_rate(puce.bandwidth);
		txcvr.set_tx_gain_soft(puce.txgain_dB);
		txcvr.set_tx_gain_uhd(puce.uhd_txgain_dB);
        if (verbose) printf("Modulation scheme: %s\n", puce.modScheme);
        modulation_scheme ms = convertModScheme(puce.modScheme, &ce.bitsPerSym);

        // Set Cyclic Redundency Check Scheme
        //crc_scheme check = convertCRCScheme(ce.crcScheme);

        // Set inner forward error correction scheme
        if (verbose) printf("Inner FEC: ");
        fec_scheme fec0 = convertFECScheme(puce.innerFEC, verbose);

        // Set outer forward error correction scheme
        if (verbose) printf("Outer FEC: ");
        fec_scheme fec1 = convertFECScheme(puce.outerFEC, verbose);
		int on = 1;
		std::clock_t time = 0;
		start = std::clock();
		//srand(std::clock());
		double primarybasebursttime = primarybursttime;
		double primarybaseresttime = primaryresttime;
		for(int o = 0; o<totaltime; ++o){
			//printf("%d\n", primarymsgnumber);
			primarybursttime = primarybasebursttime + rand() % primaryburstrandom;
			primaryresttime = primarybaseresttime + rand() % primaryrestrandom;
			int on = 1;
			time = 0;
			start = std::clock();
			a=1.0;
			printf("Cycle %d\n", o+1);
			printf("PU transmitting\n");
			mess.number = primarymsgnumber;
			mess.purpose = 't';
			//printf("%d %d\n", rxCBs.client, bfi.client);
			write(rxCBs.client, (const void*)&mess, sizeof(mess));
			primarymsgnumber++;
			//printf("%d\n", mess.number);
			//For some reason time is about 5 times slower in this while loop
			while(primarybursttime/timedivisor > time){
				//printf("Primary time %d\n", CLOCKS_PER_SEC);
				//printf("%f\n", (float)time);
		   		txcvr.end_transmit_frame();
				current = std::clock();
				time = ((float)(current-start))/CLOCKS_PER_SEC;
				txcvr.assemble_frame(header, payload, puce.payloadLen, ms, fec0, fec1);
				//current = std::clock();
				//time = (current-start)/CLOCKS_PER_SEC;
				int isLastSymbol = 0;
				while(!isLastSymbol) //&& primarybursttime > time)
					{
					isLastSymbol = txcvr.write_symbol();
					//current = std::clock();
					//time = (current-start)/CLOCKS_PER_SEC;
					//printf("%f\n", (float)time);
					//enactScenarioBaseband(txcvr.fgbuffer, ce, sc);
					txcvr.transmit_symbol();
					}
				usleep(100);
		   		txcvr.end_transmit_frame();
				current = std::clock();
				time = ((float)(current-start))/CLOCKS_PER_SEC;
			}
			on = 0;
			time = 0;
			start = std::clock();
			printf("PU resting\n");
			a=2.0;
			mess.number = primarymsgnumber;
			mess.purpose = 'r';
			write(rxCBs.client, (const void*)&mess, sizeof(mess));
			primarymsgnumber++;
			//printf("%d\n", mess.number);
			while(primaryresttime>time){
				//printf("Resting time %d\n", CLOCKS_PER_SEC);
				//printf("%f\n", (float)time);
				current = std::clock();
				time = (current-start)/CLOCKS_PER_SEC;
			}
		}
		mess.purpose = 'F';
		write(rxCBs.client, (const void*)&mess, sizeof(mess));
	}

	//If it is a secondary user then the node acts as a secondary transmitter
	//Either sensing for the primary user or transmitting with small pauses for sening
	if(secondary == 1){
		mess.type = 'S';
		rxCBs.usrptype = 'S';
		mess.msgreceived = 1;
		verbose = 0;
		printf("secondary\n");

		//The secondary user has a payload of all zeroes
		for(int h = 0; h<8; h++){
			header[h] = 0;
		};
		for(int h = 0; h<suce.payloadLen; h++){
			payload[h] = 0;
		};
		std::clock_t start;
		std::clock_t current;
		unsigned char * p = NULL;   // default subcarrier allocation
		if (verbose) 
		printf("Using ofdmtxrx\n");
		printf("%d %d %d\n", suce.numSubcarriers, suce.CPLen, suce.taperLen);

		//Sets up transceiver object
		ofdmtxrx txcvr(suce.numSubcarriers, suce.CPLen, suce.taperLen, p, dsaCallback, (void*) &rxCBs);
		txcvr.set_tx_freq(suce.frequency);
		txcvr.set_tx_rate(suce.bandwidth);
		txcvr.set_tx_gain_soft(suce.txgain_dB);
		txcvr.set_tx_gain_uhd(suce.uhd_txgain_dB);
    	txcvr.set_rx_freq(frequency);
   		txcvr.set_rx_rate(bandwidth);
    	txcvr.set_rx_gain_uhd(uhd_rxgain);
		txcvr.start_rx();
	
		int on = 1;
		float time = 0;	
		int cantransmit = 0;
		start = std::clock();
		while(true)
			{
			int on = 1;
			time = 0;
			start = std::clock();
			printf("SU transmitting\n");
			mess.number = secondarymsgnumber;
			mess.purpose = 't';
			write(rxCBs.client, (const void*)&mess, sizeof(mess));
			//printf("%d\n", mess.number);
			secondarymsgnumber++;

			//If it does not sense the primary user then the secondary user will transmit
			while(rxCBs.primaryon==0)
				{
				//printf("%d\n", rxCBs.primaryon);
				//if (verbose) printf("Modulation scheme: %s\n", ce.modScheme);
				modulation_scheme ms = convertModScheme(suce.modScheme, &suce.bitsPerSym);

				// Set Cyclic Redundency Check Scheme
				//crc_scheme check = convertCRCScheme(ce.crcScheme);

				// Set inner forward error correction scheme
				//if (verbose) printf("Inner FEC: ");
				fec_scheme fec0 = convertFECScheme(suce.innerFEC, verbose);

				// Set outer forward error correction scheme
				//if (verbose) printf("Outer FEC: ");
				fec_scheme fec1 = convertFECScheme(suce.outerFEC, verbose);
				usleep(1);
				txcvr.assemble_frame(header, payload, suce.payloadLen, ms, fec0, fec1);
				int isLastSymbol = 0;
				while(!isLastSymbol) //&& rxCBs.primaryon==0)
					{
					usleep(1);
					//printf("%d\n", rxCBs.primaryon);
					isLastSymbol = txcvr.write_symbol();
					//enactScenarioBaseband(txcvr.fgbuffer, ce, sc);
					txcvr.transmit_symbol();
					}
		   		txcvr.end_transmit_frame();
				time = 0.0;
				txcvr.start_rx();
				start = std::clock();

				//The secondary user will wait in this while loop and wait and see if any
				//primary users appear
				while(0.5 > (float)time) //&& rxCBs.primaryon == 0)
					{
					//printf("%f\n", time);
					//printf("SU transmitting\n");
					//printf("%ju\n", (uintmax_t)time);
					//printf("%d\n", rxCBs.primaryon);
					current = std::clock();
					time = ((float)(current-start))/CLOCKS_PER_SEC;
					}
				}
			time = 0;
			start = std::clock();
			std::clock_t current;
			printf("SU sensing\n");

			//Once the primary user is detected the secondary user stops transmitting
			//and switches to sensing in a new while loop
			mess.number = secondarymsgnumber;
			mess.purpose = 'r';
			write(rxCBs.client, (const void*)&mess, sizeof(mess));
			secondarymsgnumber++;
			//printf("%d\n", mess.number);
			while(rxCBs.primaryon==1)
				{
				//printf("%d\n", rxCBs.primaryon);
				time = 0;
				rxCBs.primaryon = 0;
				start = std::clock();
				std::clock_t current;

				//The while loop sets primaryon to 0 in the beginning. If the loop
				//finishes without a new primary transmission switching it to 1 then
				//the secondary user will assume it has stopped and resume transmitting
				//This while loop below will run for secondaryscantime seconds
				while(secondaryscantime > time)
					{
					//printf("scanning\n");
					//printf("%ju\n", (uintmax_t)time);
					//printf("%d\n", rxCBs.primaryon);
					current = std::clock();
					time = (current-start)/CLOCKS_PER_SEC;
					}			
				}
			}
		};
	return 0;
	
}

//If a receiver is being used but not DSA then a basic receiver is made. It does nothing
//but wait to receive transmissions and execute the dsaCallback function
if(receiver == 1 && dsa != 1){
	ce = CreateCognitiveEngine();
	readCEConfigFile(&ce, "ce1.txt", verbose);
	printf("receiver\n");
	int u;
	unsigned char * p = NULL;   // default subcarrier allocation
	if (verbose) 
	printf("Using ofdmtxrx\n");

	//Basic transceiver setup
	printf("%d %d %d\n", ce.numSubcarriers, ce.CPLen, ce.taperLen);
	ofdmtxrx txcvr(ce.numSubcarriers, ce.CPLen, ce.taperLen, p, dsaCallback, (void*) &rxCBs);
	txcvr.set_tx_freq(ce.frequency);
	txcvr.set_tx_rate(ce.bandwidth);
	txcvr.set_tx_gain_soft(ce.txgain_dB);
	txcvr.set_tx_gain_uhd(ce.uhd_txgain_dB);
    txcvr.set_rx_freq(frequency);
    txcvr.set_rx_rate(bandwidth);
    txcvr.set_rx_gain_uhd(uhd_rxgain);
	txcvr.start_rx();

	//The receiver sits in this infinite while loop and does nothing but wait to receive
	//liquid frames that it will interpret with dsaCallback
	while(true){
		u=1;
	}
	return 0;

}

//If a receiver is being used but not DSA then a basic receiver is made. It does nothing
//but wait to receive transmissions and execute the dsaCallback function
if(receiver == 1 && dsa == 1 && primary == 1){
	struct message mess;
	mess.type = 'p';
	rxCBs.usrptype = 'p';
	ce = CreateCognitiveEngine();
	readCEConfigFile(&ce, "ce1.txt", verbose);
	printf("receiver\n");
	int u;
	unsigned char * p = NULL;   // default subcarrier allocation
	if (verbose) 
	printf("Using ofdmtxrx\n");

	//Basic transceiver setup
	printf("%d %d %d\n", ce.numSubcarriers, ce.CPLen, ce.taperLen);
	ofdmtxrx txcvr(ce.numSubcarriers, ce.CPLen, ce.taperLen, p, dsaCallback, (void*) &rxCBs);
	txcvr.set_tx_freq(ce.frequency);
	txcvr.set_tx_rate(ce.bandwidth);
	txcvr.set_tx_gain_soft(ce.txgain_dB);
	txcvr.set_tx_gain_uhd(ce.uhd_txgain_dB);
    txcvr.set_rx_freq(frequency);
    txcvr.set_rx_rate(bandwidth);
    txcvr.set_rx_gain_uhd(uhd_rxgain);
	txcvr.start_rx();

	//The receiver sits in this infinite while loop and does nothing but wait to receive
	//liquid frames that it will interpret with dsaCallback
	while(true){
		u=1;
	}
	return 0;

}

//If DSA is used and a receiver is used then a DSA receiver is made
//This is a receiver that the secondary transmitter will broadcast to
//Typically this DSA receiver will only sit and receive, unless it senses the
//primary user. Then it will send a warning message to the secondary transmitter to tell
//it to switch to scanning mode
if(dsa== 1 && receiver == 1 && secondary==1){
	struct message mess;
	printf("DSA receiver\n");
	mess.type = 's';
	rxCBs.usrptype = 's';
	//The DSA receiver has a header and payload of all 2's for easy identification
	for(int h = 0; h<8; h++){
		header[h] = 2;
	};
	for(int h = 0; h<ce.payloadLen; h++){
		payload[h] = 2;
	};
	ce = CreateCognitiveEngine();
	readCEConfigFile(&ce, "ce1.txt", verbose);
	int u;
	unsigned char * p = NULL;   // default subcarrier allocation
	if (verbose) 
	printf("Using ofdmtxrx\n");
	printf("%d %d %d\n", ce.numSubcarriers, ce.CPLen, ce.taperLen);
	ofdmtxrx txcvr(ce.numSubcarriers, ce.CPLen, ce.taperLen, p, dsaCallback, (void*) &rxCBs);
	txcvr.set_tx_freq(ce.frequency);
	txcvr.set_tx_rate(ce.bandwidth);
	txcvr.set_tx_gain_soft(ce.txgain_dB);
	txcvr.set_tx_gain_uhd(ce.uhd_txgain_dB);
    txcvr.set_rx_freq(frequency);
    txcvr.set_rx_rate(bandwidth);
    txcvr.set_rx_gain_uhd(uhd_rxgain);
	txcvr.start_rx();

	//variable that determines whether it has received a secondary transmission or not
	rxCBs.secondarysending = 0;
	while(true){

		//First the receiver will be in a while loop where it does nothing but waits to
		//receive secondary transmissions
		//It will leave the while loop if it sense the primary user
		printf("Receiving from secondary user\n");
		while(rxCBs.primaryon == 0);{
			u=1;
			}


		txcvr.start_rx();
		std::clock_t time = 0;
		std::clock_t start;
		std::clock_t current;

		//After the primary user has been detected the receiver enters  new while loop where
		//it transmits a warning message to the secondary transmitter, then waits to see if
		//it receives any more primary transmissions. If it receives any more in its wait
		//time interval then it will send another warning message and continue sensing
		//If it receives no other primary transmissions it will exit the while loop and
		//wait to receive secondary transmissions
		printf("Scanning for Primary User\n");
		while(rxCBs.primaryon == 1){ //&& rxCBs.secondarysending ==0){

			printf("Sending warning to secondary user\n");
			if (verbose) printf("Modulation scheme: %s\n", ce.modScheme);
			modulation_scheme ms = convertModScheme(ce.modScheme, &ce.bitsPerSym);

			// Set Cyclic Redundency Check Scheme
			//crc_scheme check = convertCRCScheme(ce.crcScheme);

			// Set inner forward error correction scheme
			if (verbose) printf("Inner FEC: ");
			fec_scheme fec0 = convertFECScheme(ce.innerFEC, verbose);

			// Set outer forward error correction scheme
			if (verbose) printf("Outer FEC: ");
			fec_scheme fec1 = convertFECScheme(ce.outerFEC, verbose);
			txcvr.assemble_frame(header, payload, ce.payloadLen, ms, fec0, fec1);
			int isLastSymbol = 0;
			while(!isLastSymbol){
				isLastSymbol = txcvr.write_symbol();
				//enactScenarioBaseband(txcvr.fgbuffer, ce, sc);
				txcvr.transmit_symbol();
				}
	   		txcvr.end_transmit_frame();
			rxCBs.secondarysending = 0;

			time = 0;
			rxCBs.primaryon = 0;
			start = std::clock();
			while(1>time){
				//printf("%ju\n", (uintmax_t)time);
				current = std::clock();
				time = (current-start)/CLOCKS_PER_SEC;
			}
		}
			

	}
	//return 0;

}

if(dsa && isController){
	//int * clientlist;
	//int clientlistlength = 0;
	int latestprimary = 0;
	int latestsecondary = 0;
	int totalfalsealarm = 0;
	int totalmissedhole = 0;
	int totalcycles = 0;
	//int index = 0;
	int fblistlength = 10;
	struct feedbackStruct fblist[fblistlength];
	int feedbacknum[fblistlength];

	for(int o; o<fblistlength; ++o){
		fblist[o].header_valid = 0;
		fblist[o].payload_valid = 0;
	   	fblist[o].payload_len = 0;
		fblist[o].payloadByteErrors = 0;
	   	fblist[o].payloadBitErrors = 0;
		fblist[o].iteration = 0;
	   	fblist[o].evm = 0.0;
		fblist[o]. rssi = 0.0;
		fblist[o].cfo = 0.0;
		fblist[o].block_flag = 0;
		feedbacknum[o] = 0;
	}
		
	std::clock_t primaryofftime = 0;
	std::clock_t primaryontime = 0;
	std::clock_t secondaryofftime = 0;
	std::clock_t secondaryontime = 0;
	std::clock_t evacuationtime;
	std::clock_t rendevoustime;
	int primary = 0;
	//int secondary = 0;
	std::clock_t time = std::clock();
	int loop = 1;
	while(loop){
		if(msg.msgreceived == 1){
			if(msg.type == 'P'){
				if(latestprimary<msg.number){
					if(msg.purpose == 't'){
						primary = 1;
						latestprimary = msg.number;
						time = std::clock();
						primaryontime = time;
						printf("Primary user started transmitting at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
						//printf("Primary number: %d Secondar number %d\n", latestprimary, latestsecondary);
					}
					if(msg.purpose == 'r'){
						primary = 0;
						latestprimary = msg.number;
						time = std::clock();
						primaryofftime = time;
						++totalcycles;
						printf("Primary user stopped transmitting at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
					}
					if(msg.purpose == 'F'){
						primary = 0;
						latestprimary = msg.number;
						loop = 0;
						printf("Testing Over!!!\n");
					}
					if(msg.purpose == 'f'){
						
						latestprimary = msg.number;
						
						printf("Primary feedback!!!\n");
						feedbackStruct_print(&msg.feed);
					}
				}
			}
			if(msg.type == 'S'){
				if(latestsecondary<msg.number){
					if(msg.purpose == 't'){
						secondary = 1;
						latestsecondary = msg.number;
						time = std::clock();
						secondaryontime = time;
						printf("Secondary user started transmitting at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));;
						if(primary == 0){
							rendevoustime = secondaryontime - primaryofftime;
							printf("Rendevous time = %f seconds\n", ((float)rendevoustime/CLOCKS_PER_SEC));
						}
						if(primary == 1){
							printf("False Alarm\n");
							++totalfalsealarm;
						}
							
					}
					if(msg.purpose == 'r'){
						secondary = 0;
						latestsecondary = msg.number;
						time = std::clock();
						secondaryofftime = time;
						printf("Secondary user stopped transmitting at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
						//printf("Primary number: %d Secondar number %d\n", latestprimary, latestsecondary);
						if(primary==1){
							evacuationtime = secondaryofftime - primaryontime;
							printf("Evacuation time = %f seconds\n", ((float)evacuationtime/CLOCKS_PER_SEC));\
						}
						if(primary == 0){
							printf("Wasted Spectrum Hole\n");
							++totalmissedhole;
						}
					}
				}
			}
			/*if(msg.type == 'p'){
				index = finder(clientlist, &clientlistlength, msg.client); 
				if(msg.purpose == 'P'){
					fblist[index] = feedbackadder(fblist[index], msg.feed);
					time = std::clock();
					printf("Primary receiver received primary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
				}
				if(msg.purpose == 'S'){;
					time = std::clock();
					printf("Primary receiver received secondary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
				}
				if(msg.purpose == 'f'){;
					time = std::clock();
					printf("Received feedback from primary receiver with primary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
				}
				if(msg.purpose == 'F'){;
					time = std::clock();
					printf("Received feedback from primary receiver with secondary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
				}
			}
			if(msg.type == 's'){
				if(msg.purpose == 'P'){
					time = std::clock();
					printf("Secondary receiver received primary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
				}
				if(msg.purpose == 'S'){;
					time = std::clock();
					printf("Secondary receiver received secondary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
				}
				if(msg.purpose == 'f'){;
					time = std::clock();
					printf("Received feedback from secondary receiver with primary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
				}
				if(msg.purpose == 'F'){;
					time = std::clock();
					printf("Received feedback from secondary receiver with secondary transmission at time %f seconds\n", ((float)time/CLOCKS_PER_SEC));
				}
			}*/
			

		msg.msgreceived = 0;
		}
	};
	printf("Testing Complete\n");
	return 1;
}

return 0;
}

