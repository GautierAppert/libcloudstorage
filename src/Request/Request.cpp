/*****************************************************************************
 * Request.cpp : Request implementation
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "Request.h"

#include "CloudProvider/CloudProvider.h"
#include "HttpCallback.h"
#include "Utility/Utility.h"

const auto AUTHORIZE_WAIT_INTERVAL = std::chrono::milliseconds(100);

namespace cloudstorage {

Request::Request(std::shared_ptr<CloudProvider> provider)
    : provider_(provider), is_cancelled_(false) {}

void Request::set_resolver(Request::Resolver resolver) {
  function_ = std::async(std::launch::async, std::bind(resolver, this));
}

void Request::finish() {
  std::shared_future<void> future = function_;
  if (future.valid()) future.get();
}

void Request::cancel() {
  set_cancelled(true);
  finish();
}

std::unique_ptr<HttpCallback> Request::httpCallback(
    std::function<void(uint32_t, uint32_t)> progress_download,
    std::function<void(uint32_t, uint32_t)> progress_upload) {
  return make_unique<HttpCallback>(is_cancelled_, progress_download,
                                   progress_upload);
}

bool Request::reauthorize() {
  if (is_cancelled()) return false;
  std::unique_lock<std::mutex> current_authorization(
      provider()->current_authorization_mutex_);
  if (!provider()->current_authorization_) {
    provider()->current_authorization_ = provider()->authorizeAsync();
    provider()->current_authorization_status_ =
        CloudProvider::AuthorizationStatus::InProgress;
  }
  while (!is_cancelled() &&
         provider()->current_authorization_status_ ==
             CloudProvider::AuthorizationStatus::InProgress)
    provider()->authorized_.wait_for(current_authorization,
                                     AUTHORIZE_WAIT_INTERVAL);
  bool ret =
      is_cancelled() ? false : provider()->current_authorization_status_ ==
                                   CloudProvider::AuthorizationStatus::Success;
  provider()->current_authorization_ = nullptr;
  provider()->current_authorization_status_ =
      CloudProvider::AuthorizationStatus::None;
  return ret;
}

void Request::error(int, const std::string&) {}

std::string Request::error_string(int code, const std::string& desc) const {
  std::stringstream stream;
  if (HttpRequest::isCurlError(code))
    stream << "CURL code " << code << ": "
           << curl_easy_strerror(static_cast<CURLcode>(code));
  else
    stream << "HTTP code " << code << ": " << desc;
  return stream.str();
}

int Request::sendRequest(
    std::function<std::shared_ptr<HttpRequest>(std::ostream&)> factory,
    std::ostream& output, ProgressFunction download, ProgressFunction upload) {
  std::stringstream input, error_stream;
  auto request = factory(input);
  if (request) provider()->authorizeRequest(*request);
  int code =
      send(request.get(), input, output, &error_stream, download, upload);
  if (HttpRequest::isSuccess(code)) return code;
  if (provider()->reauthorize(code)) {
    if (!reauthorize()) {
      if (!is_cancelled()) this->error(code, error_stream.str());
    } else {
      request = factory(input);
      if (request) provider()->authorizeRequest(*request);
      std::stringstream error_stream;
      code =
          send(request.get(), input, output, &error_stream, download, upload);
      if (!is_cancelled() && !HttpRequest::isSuccess(code))
        this->error(code, error_stream.str());
    }
  } else {
    if (!is_cancelled() && code != HttpRequest::Aborted)
      this->error(code, error_stream.str());
  }
  return code;
}

int Request::send(HttpRequest* request, std::istream& input,
                  std::ostream& output, std::ostream* error,
                  ProgressFunction download, ProgressFunction upload) {
  if (!request) return HttpRequest::Aborted;
  int code;
  try {
    code = request->send(input, output, error, httpCallback(download, upload));
  } catch (const HttpException& e) {
    code = e.code();
  }
  return code;
}

}  // namespace cloudstorage
