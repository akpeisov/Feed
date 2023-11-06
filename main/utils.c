// utils.c
#include <stdio.h>
#include <esp_timer.h>
#include <time.h>
#include <sys/time.h>
#include "string.h"

char *getUpTime() {
	char * res = NULL;
	res = malloc(25);
	if (res != NULL) {
	    uint32_t uptime = esp_timer_get_time() / 1000;
	    unsigned long secs=uptime/1000, mins=secs/60;
	    unsigned int hours=mins/60, days=hours/24;
	    uptime-=secs*1000;
	    secs-=mins*60;
	    mins-=hours*60;
	    hours-=days*24;	        	
    	sprintf(res, "%d days %2.2d:%2.2d:%2.2d.%3.3d", (char)days, (char)hours, (char)mins, (char)secs, (int)uptime);    
	}
	return res;
}

char *getCurrentDateTime(const char *format) {
    #define LEN 21
    char * res = NULL;
	res = malloc(LEN);
	if (res != NULL) {
		time_t now;
	    struct tm timeinfo;
	    time(&now);	
	    localtime_r(&now, &timeinfo);
	    strftime(res, LEN, format, &timeinfo); //"%d.%m.%Y %H:%M:%S"
	}
    return res;
}

uint8_t isIp_v4(char *ip)
{
    int num;
    int flag = 1;
    int counter = 0;
    char *dup = strdup(ip);
    char *p = strtok(dup, ".");

    while ( p && flag )
    {
        num = atoi(p);
        if (num >= 0 && num <= 255 && (counter++ < 4))
        {
            flag = 1;
            p = strtok(NULL, ".");
        }
        else
        {
            flag = 0;
            break;
        }
    }

    free(dup);
    return flag && (counter == 4);
}

void rtrim( char * string, char * trim )
{
     // делаем обрезку справа
     int i;
     for( i = strlen (string) - 1; 
           i >= 0 && strchr (trim, string[i]) != NULL;
           i-- )
     {  
         // переставляем терминатор строки 
         string[i] = '\0';
     }
}
 
void ltrim( char * string, char * trim )
{
     // делаем обрезку слева
     while ( string[0] != '\0' && strchr ( trim, string[0] ) != NULL )
     {
         memmove( &string[0], &string[1], strlen(string) );
     }
}