// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>

#include "rgw_common.h"
#include "rgw_http_client.h"

#define dout_subsys ceph_subsys_rgw

static size_t receive_http_header(void *ptr, size_t size, size_t nmemb, void *_info)
{
  RGWHTTPClient *client = static_cast<RGWHTTPClient *>(_info);
  size_t len = size * nmemb;
  int ret = client->receive_header(ptr, size * nmemb);
  if (ret < 0) {
    dout(0) << "WARNING: client->receive_header() returned ret=" << ret << dendl;
  }

  return len;
}

static size_t receive_http_data(void *ptr, size_t size, size_t nmemb, void *_info)
{
  RGWHTTPClient *client = static_cast<RGWHTTPClient *>(_info);
  size_t len = size * nmemb;
  int ret = client->receive_data(ptr, size * nmemb);
  if (ret < 0) {
    dout(0) << "WARNING: client->receive_data() returned ret=" << ret << dendl;
  }

  return len;
}

static size_t send_http_data(void *ptr, size_t size, size_t nmemb, void *_info)
{
  RGWHTTPClient *client = static_cast<RGWHTTPClient *>(_info);
  int ret = client->send_data(ptr, size * nmemb);
  if (ret < 0) {
    dout(0) << "WARNING: client->receive_data() returned ret=" << ret << dendl;
  }

  return ret;
}

static curl_slist *headers_to_slist(list<pair<string, string> >& headers)
{
  curl_slist *h = NULL;

  list<pair<string, string> >::iterator iter;
  for (iter = headers.begin(); iter != headers.end(); ++iter) {
    pair<string, string>& p = *iter;
    string val = p.first;

    if (strncmp(val.c_str(), "HTTP_", 5) == 0) {
      val = val.substr(5);
    }

    /* we need to convert all underscores into dashes as some web servers forbid them
     * in the http header field names
     */
    for (size_t i = 0; i < val.size(); i++) {
      if (val[i] == '_') {
        val[i] = '-';
      }
    }

    val.append(": ");
    val.append(p.second);
    h = curl_slist_append(h, val.c_str());
  }

  return h;
}

int RGWHTTPClient::process(const char *method, const char *url)
{
  int ret = 0;
  CURL *curl_handle;

  char error_buf[CURL_ERROR_SIZE];

  curl_handle = curl_easy_init();

  dout(20) << "sending request to " << url << dendl;

  curl_slist *h = headers_to_slist(headers);

  curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(curl_handle, CURLOPT_URL, url);
  curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, receive_http_header);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEHEADER, (void *)this);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, receive_http_data);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)this);
  curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, (void *)error_buf);
  if (h) {
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, (void *)h);
  }
  curl_easy_setopt(curl_handle, CURLOPT_READFUNCTION, send_http_data);
  curl_easy_setopt(curl_handle, CURLOPT_READDATA, (void *)this);
  curl_easy_setopt(curl_handle, CURLOPT_UPLOAD, 1L); 
  if (has_send_len) {
    curl_easy_setopt(curl_handle, CURLOPT_INFILESIZE, (void *)send_len); 
  }
  CURLcode status = curl_easy_perform(curl_handle);
  if (status) {
    dout(0) << "curl_easy_performed returned error: " << error_buf << dendl;
    ret = -EINVAL;
  }
  curl_easy_cleanup(curl_handle);
  curl_slist_free_all(h);

  return ret;
}

struct multi_req_data {
  CURL *easy_handle;
  curl_slist *h;
  uint64_t id;

  multi_req_data() : easy_handle(NULL), h(NULL), id((uint64_t)-1) {}
  ~multi_req_data() {
    if (easy_handle)
      curl_easy_cleanup(easy_handle);

    if (h)
      curl_slist_free_all(h);
  }
};

int RGWHTTPClient::init_request(const char *method, const char *url, void *handle)
{
  CURL *easy_handle;
  multi_req_data *req_data = static_cast<multi_req_data *>(handle);

  char error_buf[CURL_ERROR_SIZE];

  easy_handle = curl_easy_init();

  req_data->easy_handle = easy_handle;

  dout(20) << "sending request to " << url << dendl;

  curl_slist *h = headers_to_slist(headers);

  req_data->h = h;

  curl_easy_setopt(easy_handle, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(easy_handle, CURLOPT_URL, url);
  curl_easy_setopt(easy_handle, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(easy_handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy_handle, CURLOPT_HEADERFUNCTION, receive_http_header);
  curl_easy_setopt(easy_handle, CURLOPT_WRITEHEADER, (void *)this);
  curl_easy_setopt(easy_handle, CURLOPT_WRITEFUNCTION, receive_http_data);
  curl_easy_setopt(easy_handle, CURLOPT_WRITEDATA, (void *)this);
  curl_easy_setopt(easy_handle, CURLOPT_ERRORBUFFER, (void *)error_buf);
  if (h) {
    curl_easy_setopt(easy_handle, CURLOPT_HTTPHEADER, (void *)h);
  }
  curl_easy_setopt(easy_handle, CURLOPT_READFUNCTION, send_http_data);
  curl_easy_setopt(easy_handle, CURLOPT_READDATA, (void *)this);
  curl_easy_setopt(easy_handle, CURLOPT_UPLOAD, 1L); 
  if (has_send_len) {
    curl_easy_setopt(easy_handle, CURLOPT_INFILESIZE, (void *)send_len); 
  }
  curl_easy_setopt(easy_handle, CURLOPT_PRIVATE, (void *)req_data);

  return 0;
}

#if HAVE_CURL_MULTI_WAIT

static int do_curl_wait(CephContext *cct, CURLM *handle)
{
  int num_fds;
  int ret = curl_multi_wait(handle, NULL, 0, cct->_conf->rgw_curl_wait_timeout_ms, &num_fds);
  if (ret) {
    dout(0) << "ERROR: curl_multi_wait() returned " << ret << dendl;
    return -EIO;
  }
  return 0;
}

#else

static int do_curl_wait(CephContext *cct, CURLM *handle)
{
  fd_set fdread;
  fd_set fdwrite;
  fd_set fdexcep;
  int maxfd = -1;
 
  FD_ZERO(&fdread);
  FD_ZERO(&fdwrite);
  FD_ZERO(&fdexcep);

  /* get file descriptors from the transfers */ 
  int ret = curl_multi_fdset(handle, &fdread, &fdwrite, &fdexcep, &maxfd);
  if (ret) {
    dout(0) << "ERROR: curl_multi_fdset returned " << ret << dendl;
    return -EIO;
  }

  /* forcing a strict timeout, as the returned fdsets might not reference all fds we wait on */
  uint64_t to = cct->_conf->rgw_curl_wait_timeout_ms;
#define RGW_CURL_TIMEOUT 1000
  if (!to)
    to = RGW_CURL_TIMEOUT;
  struct timeval timeout;
  timeout.tv_sec = to / 1000;
  timeout.tv_usec = to % 1000;

  ret = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout);
  if (ret < 0) {
    ret = -errno;
    dout(0) << "ERROR: select returned " << ret << dendl;
    return ret;
  }

  return 0;
}

#endif

RGWHTTPManager::RGWHTTPManager(CephContext *_cct) : cct(_cct), reqs_lock("RGWHTTPManager::reqs_lock"), num_reqs(0) {
  multi_handle = (void *)curl_multi_init();
}

RGWHTTPManager::~RGWHTTPManager() {
  if (multi_handle)
    curl_multi_cleanup((CURLM *)multi_handle);
}

void RGWHTTPManager::register_request(void *handle)
{
  multi_req_data *req_data = static_cast<multi_req_data *>(handle);

  RWLock::WLocker rl(reqs_lock);
  req_data->id = num_reqs;
  reqs[num_reqs] = req_data;
  num_reqs++;
}

void RGWHTTPManager::unregister_request(void *handle)
{
  multi_req_data *req_data = static_cast<multi_req_data *>(handle);

  RWLock::WLocker rl(reqs_lock);
  map<uint64_t, void *>::iterator iter = reqs.find(req_data->id);
  if (iter == reqs.end()) {
    return;
  }
  reqs.erase(iter);
}

void RGWHTTPManager::finish_request(void *handle)
{
  multi_req_data *req_data = static_cast<multi_req_data *>(handle);
  unregister_request(handle);

  delete req_data;
}

int RGWHTTPManager::add_request(RGWHTTPClient *client, const char *method, const char *url)
{
  multi_req_data *req_data = new multi_req_data;
  void *handle = (void *)req_data;

  int ret = client->init_request(method, url, handle);
  if (ret < 0) {
    return ret;
  }

  CURLMcode mstatus = curl_multi_add_handle((CURLM *)multi_handle, req_data->easy_handle);
  if (mstatus) {
    dout(0) << "ERROR: failed on curl_multi_add_handle, status=" << mstatus << dendl;
    delete req_data;
    return -EIO;
  }

  register_request(req_data);

  return 0;
}

int RGWHTTPManager::process_requests(bool wait_for_data, bool *done)
{
  int still_running;
  int mstatus;

  do {
    if (wait_for_data) {
      int ret = do_curl_wait(cct, (CURLM *)multi_handle);
      if (ret < 0) {
        return ret;
      }
    }

    mstatus = curl_multi_perform((CURLM *)multi_handle, &still_running);
    dout(20) << "curl_multi_perform returned: " << mstatus << dendl;
    switch (mstatus) {
      case CURLM_OK:
      case CURLM_CALL_MULTI_PERFORM:
        break;
      default:
        return -EINVAL;
    }
    int msgs_left;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read((CURLM *)multi_handle, &msgs_left))) {
      if (msg->msg == CURLMSG_DONE) {
	CURL *e = msg->easy_handle;
	multi_req_data *req_data;
	curl_easy_getinfo(e, CURLINFO_PRIVATE, (void **)&req_data);
	finish_request(req_data);
        switch (msg->data.result) {
          case CURLE_OK:
            break;
          default:
            dout(20) << "ERROR: msg->data.result=" << msg->data.result << dendl;
            return -EIO;
        }
      }
    }
  } while (mstatus == CURLM_CALL_MULTI_PERFORM);

  *done = (still_running == 0);

  return 0;
}

int RGWHTTPManager::complete_requests()
{
  bool done;
  int ret;
  do {
    ret = process_requests(true, &done);
  } while (!done && !ret);

  return ret;
}


