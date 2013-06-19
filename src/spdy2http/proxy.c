/*
    This file is part of libmicrospdy
    Copyright (C) 2013 Andrey Uzunov

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file proxy.c
 * @brief   Translates incoming SPDY requests to http server on localhost.
 * 			Uses libcurl.
 * 			No error handling for curl requests.
 *      TODO:
 * - test all options!
 * - don't abort on lack of memory
 * @author Andrey Uzunov
 */
 
#include "platform.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include "microspdy.h"
#include <curl/curl.h>
#include <assert.h>
#include <getopt.h>
#include <regex.h>


struct global_options
{
  char *http_backend;
  char *cert;
  char *cert_key;
  uint16_t listen_port;
  bool verbose;
  bool curl_verbose;
  bool transparent;
  bool http10;
} glob_opt;


struct URI
{
  char * full_uri;
  char * scheme;
  char * host_and_port;
  //char * host_and_port_for_connecting;
  char * host;
  char * path;
  char * path_and_more;
  char * query;
  char * fragment;
  uint16_t port;
};


#define PRINT_INFO(msg) do{\
	printf("%i:%s\n", __LINE__, msg);\
	fflush(stdout);\
	}\
	while(0)


#define PRINT_INFO2(fmt, ...) do{\
	printf("%i\n", __LINE__);\
	printf(fmt,##__VA_ARGS__);\
	printf("\n");\
	fflush(stdout);\
	}\
	while(0)


#define PRINT_VERBOSE(msg) do{\
  if(glob_opt.verbose){\
	printf("%i:%s\n", __LINE__, msg);\
	fflush(stdout);\
	}\
  }\
	while(0)


#define PRINT_VERBOSE2(fmt, ...) do{\
  if(glob_opt.verbose){\
	printf("%i\n", __LINE__);\
	printf(fmt,##__VA_ARGS__);\
	printf("\n");\
	fflush(stdout);\
	}\
	}\
	while(0)


#define CURL_SETOPT(handle, opt, val) do{\
	int ret; \
	if(CURLE_OK != (ret = curl_easy_setopt(handle, opt, val))) \
	{ \
		PRINT_INFO2("curl_easy_setopt failed (%i = %i)", opt, ret); \
		abort(); \
	} \
	}\
	while(0)
  

#define DIE(msg) do{\
	printf("FATAL ERROR (line %i): %s\n", __LINE__, msg);\
	fflush(stdout);\
  exit(EXIT_FAILURE);\
	}\
	while(0)


int loop = 1;
CURLM *multi_handle;
int still_running = 0; /* keep number of running handles */
regex_t uri_preg;


struct Proxy
{
	char *url;
	struct SPDY_Request *request;
	struct SPDY_Response *response;
	CURL *curl_handle;
	struct curl_slist *curl_headers;
	struct SPDY_NameValue *headers;
	char *version;
	char *status_msg;
	void *http_body;
	size_t http_body_size;
	//ssize_t length;
	int status;
  bool done;
};


void
free_uri(struct URI * uri)
{
  if(NULL != uri)
  {
    free(uri->full_uri);
    free(uri->scheme);
    free(uri->host_and_port);
    //free(uri->host_and_port_for_connecting);
    free(uri->host);
    free(uri->path);
    free(uri->path_and_more);
    free(uri->query);
    free(uri->fragment);
    uri->port = 0;
    free(uri);
  }
}

int
init_parse_uri(regex_t * preg)
{
  // RFC 2396
  // ^(([^:/?#]+):)?(//([^/?#]*))?([^?#]*)(\?([^#]*))?(#(.*))?
      /*
        scheme    = $2
      authority = $4
      path      = $5
      query     = $7
      fragment  = $9
      */
  
  return regcomp(preg, "^(([^:/?#]+):)?(//([^/?#]*))?([^?#]*)(\\?([^#]*))?(#(.*))?", REG_EXTENDED);
}

void
deinit_parse_uri(regex_t * preg)
{
  regfree(preg);
}
  
int
parse_uri(regex_t * preg, char * full_uri, struct URI ** uri)
{
  int ret;
  char *colon;
  long long port;
  size_t nmatch = 10;
  regmatch_t pmatch[10];

  if (0 != (ret = regexec(preg, full_uri, nmatch, pmatch, 0)))
    return ret;
    
  *uri = malloc(sizeof(struct URI));
  if(NULL == *uri)
    return -200;
    
  (*uri)->full_uri = strdup(full_uri);
  
  asprintf(&((*uri)->scheme), "%.*s",pmatch[2].rm_eo - pmatch[2].rm_so, &full_uri[pmatch[2].rm_so]);
  asprintf(&((*uri)->host_and_port), "%.*s",pmatch[4].rm_eo - pmatch[4].rm_so, &full_uri[pmatch[4].rm_so]);
  asprintf(&((*uri)->path), "%.*s",pmatch[5].rm_eo - pmatch[5].rm_so, &full_uri[pmatch[5].rm_so]);
  asprintf(&((*uri)->path_and_more), "%.*s",pmatch[9].rm_eo - pmatch[5].rm_so, &full_uri[pmatch[5].rm_so]);
  asprintf(&((*uri)->query), "%.*s",pmatch[7].rm_eo - pmatch[7].rm_so, &full_uri[pmatch[7].rm_so]);
  asprintf(&((*uri)->fragment), "%.*s",pmatch[9].rm_eo - pmatch[9].rm_so, &full_uri[pmatch[9].rm_so]);
  
  colon = strrchr((*uri)->host_and_port, ':');
  if(NULL == colon)
  {
    (*uri)->host = strdup((*uri)->host_and_port);
    /*if(0 == strcasecmp("http", uri->scheme))
    {
      uri->port = 80;
      asprintf(&(uri->host_and_port_for_connecting), "%s:80", uri->host_and_port);
    }
    else if(0 == strcasecmp("https", uri->scheme))
    {
      uri->port = 443;
      asprintf(&(uri->host_and_port_for_connecting), "%s:443", uri->host_and_port);
    }
    else
    {
      PRINT_INFO("no standard scheme!");
      */(*uri)->port = 0;
      /*uri->host_and_port_for_connecting = strdup(uri->host_and_port);
    }*/
    return 0;
  }
  
  port = atoi(colon  + 1);
  if(port<1 || port >= 256 * 256)
  {
    free_uri(*uri);
    return -100;
  }
  (*uri)->port = port;
  asprintf(&((*uri)->host), "%.*s", (int)(colon - (*uri)->host_and_port), (*uri)->host_and_port);
  
  return 0;
}


static void catch_signal(int signal)
{
  loop = 0;
}


ssize_t
response_callback (void *cls,
						void *buffer,
						size_t max,
						bool *more)
{
	int ret;
	struct Proxy *proxy = (struct Proxy *)cls;
	void *newbody;
	
	//printf("response_callback\n");
  
  *more = true;
	
	if(!proxy->http_body_size)//nothing to write now
  {
    if(proxy->done) *more = false;
		return 0;
  }
	
	if(max >= proxy->http_body_size)
	{
		ret = proxy->http_body_size;
		newbody = NULL;
	}
	else
	{
		ret = max;
		if(NULL == (newbody = malloc(proxy->http_body_size - max)))
		{
			PRINT_INFO("no memory");
			return -1;
		}
		memcpy(newbody, proxy->http_body + max, proxy->http_body_size - max);
	}
	memcpy(buffer, proxy->http_body, ret);
	free(proxy->http_body);
	proxy->http_body = newbody;
	proxy->http_body_size -= ret;
	
  if(proxy->done && 0 == proxy->http_body_size) *more = false;
	
	return ret;
}


void
response_done_callback(void *cls,
						struct SPDY_Response *response,
						struct SPDY_Request *request,
						enum SPDY_RESPONSE_RESULT status,
						bool streamopened)
{
	(void)streamopened;
	struct Proxy *proxy = (struct Proxy *)cls;
	int ret;
	
	if(SPDY_RESPONSE_RESULT_SUCCESS != status)
	{
		printf("answer was NOT sent, %i\n",status);
	}
	if(CURLM_OK != (ret = curl_multi_remove_handle(multi_handle, proxy->curl_handle)))
	{
		PRINT_INFO2("curl_multi_remove_handle failed (%i)", ret);
	}
	curl_slist_free_all(proxy->curl_headers);
	curl_easy_cleanup(proxy->curl_handle);
	
	SPDY_destroy_request(request);
	SPDY_destroy_response(response);
	free(proxy->url);
	free(proxy);
}


static size_t
curl_header_cb(void *ptr, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct Proxy *proxy = (struct Proxy *)userp;
	char *line = (char *)ptr;
	char *name;
	char *value;
	char *status;
	int i;
	int pos;
	int ret;
  int num_values;
  const char * const * values;
  bool abort_it;
	
	//printf("curl_header_cb %s\n", line);
  
  //trailer
  if(NULL != proxy->response) return 0;

	if('\r' == line[0])
	{
		//all headers were already handled; prepare spdy frames
		if(NULL == (proxy->response = SPDY_build_response_with_callback(proxy->status,
							proxy->status_msg,
							proxy->version,
							proxy->headers,
							&response_callback,
							proxy,
							0)))
			DIE("no response");
    
		SPDY_name_value_destroy(proxy->headers);
		free(proxy->status_msg);
		free(proxy->version);
		
		if(SPDY_YES != SPDY_queue_response(proxy->request,
							proxy->response,
							true,
							false,
							&response_done_callback,
							proxy))
			DIE("no queue");
		
		return realsize;
	}
	
	pos = 0;
	if(NULL == proxy->version)
	{
		//first line from headers
		//version
		for(i=pos; i<realsize && ' '!=line[i]; ++i);
		if(i == realsize)
			DIE("error on parsing headers");
		if(NULL == (proxy->version = strndup(line, i - pos)))
        DIE("No memory");
		pos = i+1;
		
		//status (number)
		for(i=pos; i<realsize && ' '!=line[i] && '\r'!=line[i]; ++i);
		if(NULL == (status = strndup(&(line[pos]), i - pos)))
        DIE("No memory");
		proxy->status = atoi(status);
		free(status);
		if(i<realsize && '\r'!=line[i])
		{
			//status (message)
			pos = i+1;
			for(i=pos; i<realsize && '\r'!=line[i]; ++i);
			if(NULL == (proxy->status_msg = strndup(&(line[pos]), i - pos)))
        DIE("No memory");
		}
		return realsize;
	}
	
	//other lines
	//header name
	for(i=pos; i<realsize && ':'!=line[i] && '\r'!=line[i]; ++i)
		line[i] = tolower(line[i]); //spdy requires lower case
	if(NULL == (name = strndup(line, i - pos)))
        DIE("No memory");
	if(0 == strcmp(SPDY_HTTP_HEADER_CONNECTION, name)
		|| 0 == strcmp(SPDY_HTTP_HEADER_KEEP_ALIVE, name))
	{
		//forbidden in spdy, ignore
		free(name);
		return realsize;
	}
	if(i == realsize || '\r'==line[i])
	{
		//no value. is it possible?
		if(SPDY_YES != SPDY_name_value_add(proxy->headers, name, ""))
			DIE("SPDY_name_value_add failed");
		return realsize;
	}
	
	//header value
	pos = i+1;
	while(pos<realsize && isspace(line[pos])) ++pos; //remove leading space
	for(i=pos; i<realsize && '\r'!=line[i]; ++i);
	if(NULL == (value = strndup(&(line[pos]), i - pos)))
        DIE("No memory");
	if(SPDY_YES != (ret = SPDY_name_value_add(proxy->headers, name, value)))
	{
    abort_it=true;
    if(NULL != (values = SPDY_name_value_lookup(proxy->headers, name, &num_values)))
      for(i=0; i<num_values; ++i)
        if(0 == strcasecmp(value, values[i]))
        {
          abort_it=false;
          PRINT_INFO2("header appears more than once with same value '%s: %s'", name, value);
          break;
        }
    
    if(abort_it)
    {
      PRINT_INFO2("SPDY_name_value_add failed (%i) for '%s'", ret, name);
      abort();
    }
	}
	free(name);
	free(value);

	return realsize;
}


static size_t
curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct Proxy *proxy = (struct Proxy *)userp;
	
	//printf("curl_write_cb %i\n", realsize);
  
	if(NULL == proxy->http_body)
		proxy->http_body = malloc(realsize);
	else
		proxy->http_body = realloc(proxy->http_body, proxy->http_body_size + realsize);
	if(NULL == proxy->http_body)
	{
		PRINT_INFO("not enough memory (realloc returned NULL)");
		return 0;
	}

	memcpy(proxy->http_body + proxy->http_body_size, contents, realsize);
	proxy->http_body_size += realsize;

	return realsize;
}


int
iterate_cb (void *cls, const char *name, const char * const * value, int num_values)
{
	struct Proxy *proxy = (struct Proxy *)cls;
  struct curl_slist **curl_headers = (&(proxy->curl_headers));
  char *line;
  int line_len = strlen(name) + 3; //+ ": \0"
  int i;
  
  for(i=0; i<num_values; ++i)
  {
		if(i) line_len += 2; //", "
		line_len += strlen(value[i]);
	}
	
	if(NULL == (line = malloc(line_len)))//no recovery
    DIE("No memory");
	line[0] = 0;
    
    strcat(line, name);
    strcat(line, ": ");
    //all spdy header names are lower case;
    //for simplicity here we just capitalize the first letter
    line[0] = toupper(line[0]);
    
	for(i=0; i<num_values; ++i)
	{
		if(i) strcat(line, ", ");
		strcat(line, value[i]);
	}
  if(NULL == (*curl_headers = curl_slist_append(*curl_headers, line)))
		DIE("curl_slist_append failed");
	free(line);
	
	return SPDY_YES;
}


void
standard_request_handler(void *cls,
                        struct SPDY_Request * request,
                        uint8_t priority,
                        const char *method,
                        const char *path,
                        const char *version,
                        const char *host,
                        const char *scheme,
                        struct SPDY_NameValue * headers)
{
	(void)cls;
	(void)priority;
	(void)host;
	(void)scheme;
	
	struct Proxy *proxy;
	int ret;
  struct URI *uri;
	
	PRINT_VERBOSE2("received request for '%s %s %s'\n", method, path, version);
	if(NULL == (proxy = malloc(sizeof(struct Proxy))))
        DIE("No memory");
	memset(proxy, 0, sizeof(struct Proxy));
	proxy->request = request;
	if(NULL == (proxy->headers = SPDY_name_value_create()))
        DIE("No memory");
  
  if(glob_opt.transparent)
  {
    if(NULL != glob_opt.http_backend) //use always same host
      ret = asprintf(&(proxy->url),"%s://%s%s", scheme, glob_opt.http_backend, path);
    else //use host header
      ret = asprintf(&(proxy->url),"%s://%s%s", scheme, host, path);
    if(-1 == ret)
        DIE("No memory");
        
    ret = parse_uri(&uri_preg, proxy->url, &uri);
    if(ret != 0)
      DIE("parsing built uri failed");
  }
  else
  {
    ret = parse_uri(&uri_preg, path, &uri);
    PRINT_INFO2("path %s '%s' '%s'", path, uri->scheme, uri->host);
    if(ret != 0 || !strlen(uri->scheme) || !strlen(uri->host))
      DIE("parsing received uri failed");
      
    if(NULL != glob_opt.http_backend) //use backend host
    {
      ret = asprintf(&(proxy->url),"%s://%s%s", uri->scheme, glob_opt.http_backend, uri->path_and_more);
      if(-1 == ret)
        DIE("No memory");
    }
    else //use request path
      if(NULL == (proxy->url = strdup(path)))
        DIE("No memory");
  }
  
  free(uri);
  
  PRINT_VERBOSE2("curl will request '%s'", proxy->url);
    
  SPDY_name_value_iterate(headers, &iterate_cb, proxy);
	
	if(NULL == (proxy->curl_handle = curl_easy_init()))
    {
		PRINT_INFO("curl_easy_init failed");
		abort();
	}
	
	if(glob_opt.curl_verbose)
    CURL_SETOPT(proxy->curl_handle, CURLOPT_VERBOSE, 1);
	CURL_SETOPT(proxy->curl_handle, CURLOPT_URL, proxy->url);
	if(glob_opt.http10)
		CURL_SETOPT(proxy->curl_handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
	CURL_SETOPT(proxy->curl_handle, CURLOPT_WRITEFUNCTION, curl_write_cb);
	CURL_SETOPT(proxy->curl_handle, CURLOPT_WRITEDATA, proxy);
	CURL_SETOPT(proxy->curl_handle, CURLOPT_HEADERFUNCTION, curl_header_cb);
	CURL_SETOPT(proxy->curl_handle, CURLOPT_HEADERDATA, proxy);
	CURL_SETOPT(proxy->curl_handle, CURLOPT_PRIVATE, proxy);
	CURL_SETOPT(proxy->curl_handle, CURLOPT_HTTPHEADER, proxy->curl_headers);
  CURL_SETOPT(proxy->curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
  CURL_SETOPT(proxy->curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
	
	if(CURLM_OK != (ret = curl_multi_add_handle(multi_handle, proxy->curl_handle)))
	{
		PRINT_INFO2("curl_multi_add_handle failed (%i)", ret);
		abort();
	}
	
	if(CURLM_OK != (ret = curl_multi_perform(multi_handle, &still_running))
		&& CURLM_CALL_MULTI_PERFORM != ret)
	{
		PRINT_INFO2("curl_multi_perform failed (%i)", ret);
		abort();
	}
}

int
run()
{
  unsigned long long timeoutlong=0;
  long curl_timeo = -1;
	struct timeval timeout;
	int ret;
	fd_set rs;
	fd_set ws;
	fd_set es;
	fd_set curl_rs;
	fd_set curl_ws;
	fd_set curl_es;
	int maxfd = -1;
	struct SPDY_Daemon *daemon;
  CURLMsg *msg;
  int msgs_left;
  struct Proxy *proxy;

	signal(SIGPIPE, SIG_IGN);
	
  if (signal(SIGINT, catch_signal) == SIG_ERR)
    PRINT_VERBOSE("signal failed");
    
  srand(time(NULL));
  if(init_parse_uri(&uri_preg))
    DIE("Regexp compilation failed");
    
	SPDY_init();
	
	daemon = SPDY_start_daemon(glob_opt.listen_port,
								glob_opt.cert,
								glob_opt.cert_key,
								NULL,
								NULL,
								&standard_request_handler,
								NULL,
								NULL,
								SPDY_DAEMON_OPTION_SESSION_TIMEOUT,
								1800,
								SPDY_DAEMON_OPTION_END);
	
	if(NULL==daemon){
		printf("no daemon\n");
		return 1;
	}
	
	multi_handle = curl_multi_init();
	if(NULL==multi_handle)
		DIE("no multi_handle");
  
	timeout.tv_usec = 0;

	do
	{
		FD_ZERO(&rs);
		FD_ZERO(&ws);
		FD_ZERO(&es);
		FD_ZERO(&curl_rs);
		FD_ZERO(&curl_ws);
		FD_ZERO(&curl_es);

		if(still_running > 0)
			timeout.tv_sec = 0; //return immediately
		else
		{
			ret = SPDY_get_timeout(daemon, &timeoutlong);
			if(SPDY_NO == ret)
				timeout.tv_sec = 1;
			else
				timeout.tv_sec = timeoutlong;
		}
		timeout.tv_usec = 0;
		
		maxfd = SPDY_get_fdset (daemon,
								&rs,
								&ws, 
								&es);	
		assert(-1 != maxfd);
		
		ret = select(maxfd+1, &rs, &ws, &es, &timeout);
		
		switch(ret) {
			case -1:
				PRINT_INFO2("select error: %i", errno);
				break;
			case 0:
				break;
			default:
				SPDY_run(daemon);
			break;
		}
		
		timeout.tv_sec = 0;
		if(still_running > 0)
		{
			if(CURLM_OK != (ret = curl_multi_timeout(multi_handle, &curl_timeo)))
			{
				PRINT_INFO2("curl_multi_timeout failed (%i)", ret);
				abort();
			}
			if(curl_timeo >= 0 && curl_timeo < 500)
				timeout.tv_usec = curl_timeo * 1000;
			else
				timeout.tv_usec = 500000;
		}
		else continue;
		//else timeout.tv_usec = 500000;

		if(CURLM_OK != (ret = curl_multi_fdset(multi_handle, &curl_rs, &curl_ws, &curl_es, &maxfd)))
		{
			PRINT_INFO2("curl_multi_fdset failed (%i)", ret);
			abort();
		}
		if(-1 == maxfd)
		{
			PRINT_INFO("maxfd is -1");
			//continue;
			ret = 0;
		}
		else
		ret = select(maxfd+1, &curl_rs, &curl_ws, &curl_es, &timeout);

		switch(ret) {
			case -1:
				PRINT_INFO2("select error: %i", errno);
				break;
			case 0: /* timeout */
				//break or not
			default: /* action */
				if(CURLM_OK != (ret = curl_multi_perform(multi_handle, &still_running))
					&& CURLM_CALL_MULTI_PERFORM != ret)
				{
					PRINT_INFO2("curl_multi_perform failed (%i)", ret);
					abort();
				}
				break;
		}
    
    while ((msg = curl_multi_info_read(multi_handle, &msgs_left))) {
      if (msg->msg == CURLMSG_DONE) {
        if(CURLE_OK == msg->data.result)
        {
          if(CURLE_OK != (ret = curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &proxy)))
          {
            PRINT_INFO2("err %i",ret);
            abort();
          }

          proxy->done = true;
        }
        else
        {
          //TODO handle error
          PRINT_INFO("bad curl result");
        }
      }
      else PRINT_INFO("shouldn't happen");
    }
  }
  while(loop);
	
	curl_multi_cleanup(multi_handle);

	SPDY_stop_daemon(daemon);
	
	SPDY_deinit();
  
  deinit_parse_uri(&uri_preg);
	
	return 0;
}

void
display_usage()
{
  printf(
    "Usage: microspdy2http [-vh0t] [-b <HTTP-SERVER>] -p <PORT> -c <CERTIFICATE> -k <CERT-KEY>\n\n"
    "OPTIONS:\n"
    "    -p, --port            Listening port.\n"
    "    -c, --certificate     Path to a certificate file.\n"
    "    -k, --certificate-key Path to a key file for the certificate.\n"
    "    -b, --backend-server  If set, the proxy will connect always to it.\n"
    "                          Otherwise the proxy will connect to the URL\n"
    "                          which is specified in the path or 'Host:'.\n"
    "    -v, --verbose         Print debug information.\n"
    "    -h, --curl-verbose    Print debug information for curl.\n"
    "    -0, --http10          Prefer HTTP/1.0 connections to the next hop.\n"
    "    -t, --transparent     If set, the proxy will fetch an URL which\n"
    "                          is based on 'Host:' header and requested path.\n"
    "                          Otherwise, full URL in the requested path is required.\n\n"

  );
}

int
main (int argc, char *const *argv)
{	
	  
  int getopt_ret;
  int option_index;
  struct option long_options[] = {
    {"port",  required_argument, 0, 'p'},
    {"certificate",  required_argument, 0, 'c'},
    {"certificate-key",  required_argument, 0, 'k'},
    {"backend-server",  required_argument, 0, 'b'},
    {"verbose",  no_argument, 0, 'v'},
    {"curl-verbose",  no_argument, 0, 'h'},
    {"http10",  no_argument, 0, '0'},
    {"transparent",  no_argument, 0, 't'},
    {0, 0, 0, 0}
  };
  
  while (1)
  {
    getopt_ret = getopt_long( argc, argv, "p:c:k:b:v0t", long_options, &option_index);
    if (getopt_ret == -1)
      break;

    switch(getopt_ret)
    {
      case 'p':
        glob_opt.listen_port = atoi(optarg);
        break;
        
      case 'c':
        glob_opt.cert = strdup(optarg);
        if(NULL == glob_opt.cert)
          return 1;
        break;
        
      case 'k':
        glob_opt.cert_key = strdup(optarg);
        if(NULL == glob_opt.cert_key)
          return 1;
        break;
        
      case 'b':
        glob_opt.http_backend = strdup(optarg);
        if(NULL == glob_opt.http_backend)
          return 1;
        break;
        
      case 'v':
        glob_opt.verbose = true;
        break;
        
      case 'h':
        glob_opt.curl_verbose = true;
        break;
        
      case '0':
        glob_opt.http10 = true;
        break;
        
      case 't':
        glob_opt.transparent = true;
        break;
        
      case 0:
        PRINT_INFO("0 from getopt");
        break;
        
      case '?':
        display_usage();
        return 1;
        
      default:
        DIE("default from getopt");
    }
  }
  
  if(
    0 == glob_opt.listen_port
    || NULL == glob_opt.cert
    || NULL == glob_opt.cert_key
    //|| !glob_opt.transparent && NULL != glob_opt.http_backend
    )
  {
    display_usage();
    return 1;
  }
    
  return run();
}

