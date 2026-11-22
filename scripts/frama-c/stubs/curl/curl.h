// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit stub for <curl/curl.h>. The real header (libcurl4-openssl-dev)
// uses CURL_DEPRECATED macros and __int128 typedefs Frama-C 25 cannot parse.
// We declare only the API surface our fetch.c touches.
#ifndef DIORAMAP_AUDIT_CURL_H
#define DIORAMAP_AUDIT_CURL_H

#include <stddef.h>
#include <stdint.h>

typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;
typedef int CURL_CSELECT;
typedef int CURLMSG;
typedef int curl_off_t;
typedef int CURLMcode;

#define CURLE_OK 0
#define CURLOPT_URL 10002
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_TIMEOUT_MS 155
#define CURLOPT_CONNECTTIMEOUT_MS 156
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_POSTFIELDS 10015
#define CURLOPT_POSTFIELDSIZE_LARGE 30120
#define CURLOPT_HTTPGET 80
#define CURLOPT_POST 47
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_CUSTOMREQUEST 10036
#define CURLOPT_HEADERFUNCTION 20079
#define CURLOPT_HEADERDATA 10029
#define CURLOPT_FAILONERROR 45
#define CURLOPT_USERAGENT 10018
#define CURLOPT_ERRORBUFFER 10010
#define CURLOPT_CAINFO 10065
#define CURLOPT_CAPATH 10097
#define CURLINFO_RESPONSE_CODE 0x200002
#define CURLE_FAILED_INIT 2
#define CURLE_OUT_OF_MEMORY 27
#define CURLE_OPERATION_TIMEDOUT 28
#define CURLOPT_POSTFIELDSIZE 60
#define CURLOPT_VERBOSE 41
#define CURLOPT_SSL_VERIFYPEER 64
#define CURLOPT_SSL_VERIFYHOST 81
#define CURLOPT_NOPROGRESS 43
#define CURLOPT_TIMEOUT 13
#define CURLOPT_CONNECTTIMEOUT 78
#define CURLOPT_ACCEPT_ENCODING 102
#define CURLE_ABORTED_BY_CALLBACK 42
#define CURLE_COULDNT_RESOLVE_HOST 6
#define CURLE_COULDNT_RESOLVE_PROXY 5
#define CURLE_PEER_FAILED_VERIFICATION 60
#define CURLE_SSL_CONNECT_ERROR 35
#define CURLINFO_TOTAL_TIME 0x300003

#define CURL_GLOBAL_DEFAULT 0

struct curl_slist {
    char* data;
    struct curl_slist* next;
};

CURL* curl_easy_init(void);
void  curl_easy_cleanup(CURL* h);
void  curl_easy_reset(CURL* h);
CURLcode curl_easy_setopt(CURL* h, CURLoption opt, ...);
CURLcode curl_easy_perform(CURL* h);
CURLcode curl_easy_getinfo(CURL* h, CURLINFO info, ...);
const char* curl_easy_strerror(CURLcode rc);

struct curl_slist* curl_slist_append(struct curl_slist* list, const char* item);
void curl_slist_free_all(struct curl_slist* list);

CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);

// libcurl-multi pieces some callers reach for. Declare lazily; expand if
// linker errors hit a missing symbol.

#endif
