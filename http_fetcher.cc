//
// Copyright (C) 2009 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/http_fetcher.h"

#include <base/bind.h>

using base::Closure;
using chromeos::MessageLoop;
using std::deque;
using std::string;

namespace chromeos_update_engine {

HttpFetcher::~HttpFetcher() {
  if (no_resolver_idle_id_ != MessageLoop::kTaskIdNull) {
    MessageLoop::current()->CancelTask(no_resolver_idle_id_);
    no_resolver_idle_id_ = MessageLoop::kTaskIdNull;
  }
}

void HttpFetcher::SetPostData(const void* data, size_t size,
                              HttpContentType type) {
  post_data_set_ = true;
  post_data_.clear();
  const char* char_data = reinterpret_cast<const char*>(data);
  post_data_.insert(post_data_.end(), char_data, char_data + size);
  post_content_type_ = type;
}

void HttpFetcher::SetPostData(const void* data, size_t size) {
  SetPostData(data, size, kHttpContentTypeUnspecified);
}

// Proxy methods to set the proxies, then to pop them off.
bool HttpFetcher::ResolveProxiesForUrl(const string& url,
                                       const Closure& callback) {
  CHECK_EQ(static_cast<Closure*>(nullptr), callback_.get());
  callback_.reset(new Closure(callback));

  if (!proxy_resolver_) {
    LOG(INFO) << "Not resolving proxies (no proxy resolver).";
    no_resolver_idle_id_ = MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(&HttpFetcher::NoProxyResolverCallback,
                   base::Unretained(this)));
    return true;
  }
  return proxy_resolver_->GetProxiesForUrl(url,
                                           &HttpFetcher::StaticProxiesResolved,
                                           this);
}

void HttpFetcher::NoProxyResolverCallback() {
  ProxiesResolved(deque<string>());
}

void HttpFetcher::ProxiesResolved(const deque<string>& proxies) {
  no_resolver_idle_id_ = MessageLoop::kTaskIdNull;
  if (!proxies.empty())
    SetProxies(proxies);
  CHECK_NE(static_cast<Closure*>(nullptr), callback_.get());
  Closure* callback = callback_.release();
  // This may indirectly call back into ResolveProxiesForUrl():
  callback->Run();
  delete callback;
}

}  // namespace chromeos_update_engine
