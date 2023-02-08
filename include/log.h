#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#ifdef DEBUG 
#define LOG_DEBUG(format,...) printf("File: " __FILE__ ", Line: %05d: " format "\n", __LINE__, ##__VA_ARGS__)  
#else  
#define LOG_DEBUG(format,...)  
#endif  

#endif