/*****************************************************************************
 * MegaNz.cpp : Mega implementation
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

#include "MegaNz.h"

#include "IAuth.h"
#include "Request/DownloadFileRequest.h"
#include "Request/Request.h"
#include "Utility/Item.h"
#include "Utility/Utility.h"

#include <array>
#include <cstring>
#include <fstream>
#include <queue>

using namespace mega;

const int BUFFER_SIZE = 1024;
const int CACHE_FILENAME_LENGTH = 12;
const int DEFAULT_DAEMON_PORT = 12346;

namespace cloudstorage {

namespace {

class Listener {
 public:
  static constexpr int IN_PROGRESS = -1;
  static constexpr int FAILURE = 0;
  static constexpr int SUCCESS = 1;

  Listener(Semaphore* semaphore)
      : semaphore_(semaphore), status_(IN_PROGRESS) {}

  Semaphore* semaphore_;
  std::atomic_int status_;
};

class RequestListener : public mega::MegaRequestListener, public Listener {
 public:
  using Listener::Listener;

  void onRequestFinish(MegaApi*, MegaRequest* r, MegaError* e) override {
    if (e->getErrorCode() == 0)
      status_ = SUCCESS;
    else
      status_ = FAILURE;
    if (r->getLink()) link_ = r->getLink();
    node_ = r->getNodeHandle();
    semaphore_->notify();
  }

  std::string link_;
  MegaHandle node_;
};

class TransferListener : public mega::MegaTransferListener, public Listener {
 public:
  using Listener::Listener;

  bool onTransferData(MegaApi*, MegaTransfer* t, char* buffer,
                      size_t size) override {
    if (request_->is_cancelled()) return false;
    if (download_callback_) {
      download_callback_->receivedData(buffer, size);
      download_callback_->progress(t->getTotalBytes(),
                                   t->getTransferredBytes());
    }
    return true;
  }

  void onTransferUpdate(MegaApi* mega, MegaTransfer* t) override {
    if (upload_callback_)
      upload_callback_->progress(t->getTotalBytes(), t->getTransferredBytes());
    if (request_->is_cancelled()) mega->cancelTransfer(t);
  }

  void onTransferFinish(MegaApi*, MegaTransfer*, MegaError* e) override {
    if (e->getErrorCode() == 0)
      status_ = SUCCESS;
    else {
      error_ = e->getErrorString();
      status_ = FAILURE;
    }
    semaphore_->notify();
  }

  IDownloadFileCallback::Pointer download_callback_;
  IUploadFileCallback::Pointer upload_callback_;
  Request<EitherError<void>>* request_;
  std::string error_;
};

struct Buffer {
  using Pointer = std::shared_ptr<Buffer>;

  std::mutex mutex_;
  std::queue<char> data_;
};

class HttpData : public IHttpServer::IResponse::ICallback {
 public:
  HttpData(Buffer::Pointer d) : buffer_(d) {}

  ~HttpData() {
    auto p = request_->provider();
    if (p) {
      auto mega = static_cast<MegaNz*>(request_->provider().get());
      mega->removeStreamRequest(request_);
    }
  }

  int putData(char* buf, size_t max) override {
    if (request_->is_cancelled()) return -1;
    std::lock_guard<std::mutex> lock(buffer_->mutex_);
    size_t cnt = std::min(buffer_->data_.size(), max);
    for (size_t i = 0; i < cnt; i++) {
      buf[i] = buffer_->data_.front();
      buffer_->data_.pop();
    }
    return cnt;
  }

  Buffer::Pointer buffer_;
  std::shared_ptr<Request<EitherError<void>>> request_;
};

class HttpDataCallback : public IDownloadFileCallback {
 public:
  HttpDataCallback(Buffer::Pointer d) : buffer_(d) {}

  void receivedData(const char* data, uint32_t length) override {
    std::lock_guard<std::mutex> lock(buffer_->mutex_);
    for (uint32_t i = 0; i < length; i++) buffer_->data_.push(data[i]);
  }

  void done() override {}
  void error(Error) override {}
  void progress(uint32_t, uint32_t) override {}

  Buffer::Pointer buffer_;
};

}  // namespace

MegaNz::HttpServerCallback::HttpServerCallback(MegaNz* p) : provider_(p) {}

IHttpServer::IResponse::Pointer MegaNz::HttpServerCallback::receivedConnection(
    const IHttpServer& server, const IHttpServer::IConnection& connection) {
  const char* state = connection.getParameter("state");
  if (!state || state != provider_->auth()->state())
    return server.createResponse(403, {}, "state parameter missing / invalid");
  const char* file = connection.getParameter("file");
  std::unique_ptr<mega::MegaNode> node(provider_->mega()->getNodeByHandle(
      provider_->mega()->base64ToHandle(file)));
  if (!node) return server.createResponse(404, {}, "file not found");
  auto buffer = std::make_shared<Buffer>();
  auto data = util::make_unique<HttpData>(buffer);
  auto request = std::make_shared<Request<EitherError<void>>>(
      std::weak_ptr<CloudProvider>(provider_->shared_from_this()));
  data->request_ = request;
  provider_->addStreamRequest(request);
  int code = 200;
  auto extension =
      static_cast<Item*>(provider_->toItem(node.get()).get())->extension();
  std::unordered_map<std::string, std::string> headers = {
      {"Content-Type", util::to_mime_type(extension)},
      {"Accept-Ranges", "bytes"},
      {"Content-Disposition",
       "inline; filename=\"" + std::string(node->getName()) + "\""}};
  util::range range = {0, node->getSize()};
  if (const char* range_str = connection.header("Range")) {
    range = util::parse_range(range_str);
    if (range.size == -1) range.size = node->getSize() - range.start;
    if (range.start + range.size > node->getSize() || range.start == -1)
      return server.createResponse(416, {}, "invalid range");
    std::stringstream stream;
    stream << "bytes " << range.start << "-" << range.start + range.size - 1
           << "/" << node->getSize();
    headers["Content-Range"] = stream.str();
    code = 206;
  }
  request->set_resolver(provider_->downloadResolver(
      provider_->toItem(node.get()),
      util::make_unique<HttpDataCallback>(buffer), range.start, range.size));
  return server.createResponse(code, headers, range.size, BUFFER_SIZE,
                               std::move(data));
}

MegaNz::MegaNz()
    : CloudProvider(util::make_unique<Auth>()),
      mega_(),
      authorized_(),
      engine_(device_()),
      daemon_port_(DEFAULT_DAEMON_PORT),
      daemon_(),
      temporary_directory_(".") {}

MegaNz::~MegaNz() {
  daemon_ = nullptr;
  std::unordered_set<DownloadFileRequest::Pointer> requests;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    requests = stream_requests_;
    stream_requests_.clear();
  }
  for (auto r : requests) r->cancel();
}

void MegaNz::addStreamRequest(DownloadFileRequest::Pointer r) {
  std::lock_guard<std::mutex> lock(mutex_);
  stream_requests_.insert(r);
}

void MegaNz::removeStreamRequest(DownloadFileRequest::Pointer r) {
  r->cancel();
  std::lock_guard<std::mutex> lock(mutex_);
  stream_requests_.erase(r);
}

void MegaNz::initialize(InitData&& data) {
  {
    std::lock_guard<std::mutex> lock(auth_mutex());
    if (data.hints_.find("client_id") == std::end(data.hints_))
      mega_ = util::make_unique<MegaApi>("ZVhB0Czb");
    else
      setWithHint(data.hints_, "client_id", [this](std::string v) {
        mega_ = util::make_unique<MegaApi>(v.c_str());
      });
    setWithHint(data.hints_, "daemon_port",
                [this](std::string v) { daemon_port_ = std::atoi(v.c_str()); });
    setWithHint(data.hints_, "temporary_directory",
                [this](std::string v) { temporary_directory_ = v; });
    setWithHint(data.hints_, "file_url",
                [this](std::string v) { file_url_ = v; });
  }
  CloudProvider::initialize(std::move(data));
  if (file_url_.empty()) file_url_ = auth()->redirect_uri_host();
  daemon_ = http_server()->create(
      util::make_unique<HttpServerCallback>(this), auth()->state(),
      IHttpServer::Type::MultiThreaded, daemon_port_);
}

std::string MegaNz::name() const { return "mega"; }

std::string MegaNz::endpoint() const {
  return util::address(file_url_, daemon_port_);
}

IItem::Pointer MegaNz::rootDirectory() const {
  return util::make_unique<Item>("root", "/", IItem::FileType::Directory);
}

ICloudProvider::Hints MegaNz::hints() const {
  Hints result = {{"daemon_port", std::to_string(daemon_port_)},
                  {"temporary_directory", temporary_directory_}};
  auto t = CloudProvider::hints();
  result.insert(t.begin(), t.end());
  return result;
}

ICloudProvider::ExchangeCodeRequest::Pointer MegaNz::exchangeCodeAsync(
    const std::string& code, ExchangeCodeCallback callback) {
  auto r =
      util::make_unique<Request<EitherError<std::string>>>(shared_from_this());
  r->set_resolver([this, code, callback](Request<EitherError<std::string>>*) {
    auto token = authorizationCodeToToken(code);
    callback(token->token_);
    return token->token_;
  });
  return std::move(r);
}

AuthorizeRequest::Pointer MegaNz::authorizeAsync() {
  return util::make_unique<Authorize>(
      shared_from_this(), [this](AuthorizeRequest* r) -> EitherError<void> {
        if (!login(r)) {
          if (r->is_cancelled()) return Error{IHttpRequest::Aborted, ""};
          if (callback()->userConsentRequired(*this) ==
              ICloudProvider::ICallback::Status::WaitForAuthorizationCode) {
            auto code = r->getAuthorizationCode();
            {
              std::lock_guard<std::mutex> mutex(auth_mutex());
              auth()->set_access_token(authorizationCodeToToken(code));
            }
            if (!login(r)) return Error{401, "invalid credentials"};
          }
        }
        Authorize::Semaphore semaphore(r);
        RequestListener fetch_nodes_listener_(&semaphore);
        mega_->fetchNodes(&fetch_nodes_listener_);
        semaphore.wait();
        mega_->removeRequestListener(&fetch_nodes_listener_);
        if (fetch_nodes_listener_.status_ != RequestListener::SUCCESS) {
          return Error{500, "couldn't fetch nodes"};
        }
        authorized_ = true;
        return nullptr;
      });
}

ICloudProvider::GetItemDataRequest::Pointer MegaNz::getItemDataAsync(
    const std::string& id, GetItemDataCallback callback) {
  auto r = util::make_unique<Request<EitherError<IItem>>>(shared_from_this());
  r->set_resolver([id, callback,
                   this](Request<EitherError<IItem>>* r) -> EitherError<IItem> {
    if (!ensureAuthorized(r)) {
      Error e{401, "unauthorized"};
      callback(e);
      return e;
    }
    std::unique_ptr<mega::MegaNode> node(mega_->getNodeByPath(id.c_str()));
    if (!node) {
      Error e{404, "not found"};
      callback(e);
      return e;
    }
    auto item = toItem(node.get());
    callback(item);
    return item;
  });
  return std::move(r);
}

ICloudProvider::ListDirectoryRequest::Pointer MegaNz::listDirectoryAsync(
    IItem::Pointer item, IListDirectoryCallback::Pointer callback) {
  auto r = util::make_unique<Request<EitherError<std::vector<IItem::Pointer>>>>(
      shared_from_this());
  r->set_resolver([this, item, callback](
                      Request<EitherError<std::vector<IItem::Pointer>>>* r)
                      -> EitherError<std::vector<IItem::Pointer>> {
    if (!ensureAuthorized(r)) {
      Error e = {401, "authorization failed"};
      if (!r->is_cancelled())
        callback->error({401, "Authorization failed."});
      else
        e = {IHttpRequest::Aborted, ""};
      return e;
    }
    std::unique_ptr<mega::MegaNode> node(
        mega_->getNodeByPath(item->id().c_str()));
    std::vector<IItem::Pointer> result;
    if (node) {
      std::unique_ptr<mega::MegaNodeList> lst(mega_->getChildren(node.get()));
      if (lst) {
        for (int i = 0; i < lst->size(); i++) {
          auto item = toItem(lst->get(i));
          result.push_back(item);
          callback->receivedItem(item);
        }
      }
    }
    callback->done(result);
    return result;
  });
  return std::move(r);
}

ICloudProvider::DownloadFileRequest::Pointer MegaNz::downloadFileAsync(
    IItem::Pointer item, IDownloadFileCallback::Pointer callback) {
  auto r = util::make_unique<Request<EitherError<void>>>(shared_from_this());
  r->set_resolver(downloadResolver(item, callback));
  return std::move(r);
}

ICloudProvider::UploadFileRequest::Pointer MegaNz::uploadFileAsync(
    IItem::Pointer item, const std::string& filename,
    IUploadFileCallback::Pointer callback) {
  auto r = util::make_unique<Request<EitherError<void>>>(shared_from_this());
  r->set_resolver([this, item, callback, filename](
                      Request<EitherError<void>>* r) -> EitherError<void> {
    if (!ensureAuthorized(r)) {
      Error e = {401, "authorization failed"};
      if (!r->is_cancelled())
        callback->error({401, "Authorization failed."});
      else
        e = {IHttpRequest::Aborted, ""};
      return e;
    }
    std::string cache = temporaryFileName();
    {
      std::fstream mega_cache(cache.c_str(),
                              std::fstream::out | std::fstream::binary);
      if (!mega_cache) {
        callback->error({403, "Couldn't open cache file " + cache});
        return Error{403, "couldn't open cache file" + cache};
      }
      std::array<char, BUFFER_SIZE> buffer;
      while (auto length = callback->putData(buffer.data(), BUFFER_SIZE)) {
        if (r->is_cancelled()) {
          std::remove(cache.c_str());
          return Error{IHttpRequest::Aborted, ""};
        }
        mega_cache.write(buffer.data(), length);
      }
    }
    Request<EitherError<void>>::Semaphore semaphore(r);
    TransferListener listener(&semaphore);
    listener.upload_callback_ = callback;
    listener.request_ = r;
    std::unique_ptr<mega::MegaNode> node(
        mega_->getNodeByPath(item->id().c_str()));
    mega_->startUpload(cache.c_str(), node.get(), filename.c_str(), &listener);
    semaphore.wait();
    if (r->is_cancelled())
      while (listener.status_ == Listener::IN_PROGRESS) semaphore.wait();
    std::remove(cache.c_str());
    mega_->removeTransferListener(&listener);
    if (listener.status_ != Listener::SUCCESS) {
      if (!r->is_cancelled()) {
        callback->error({500, "Upload error: " + listener.error_});
        return Error{500, listener.error_};
      } else
        return Error{IHttpRequest::Aborted, ""};
    } else {
      callback->done();
      return nullptr;
    }
  });
  return std::move(r);
}

ICloudProvider::DownloadFileRequest::Pointer MegaNz::getThumbnailAsync(
    IItem::Pointer item, IDownloadFileCallback::Pointer callback) {
  auto r = util::make_unique<Request<EitherError<void>>>(shared_from_this());
  r->set_resolver([this, item, callback](
                      Request<EitherError<void>>* r) -> EitherError<void> {
    if (!ensureAuthorized(r)) {
      Error e = {401, "authorization failed"};
      if (!r->is_cancelled())
        callback->error({401, "Authorization failed."});
      else
        e = {IHttpRequest::Aborted, ""};
      return e;
    }
    Request<EitherError<void>>::Semaphore semaphore(r);
    RequestListener listener(&semaphore);
    std::string cache = temporaryFileName();
    std::unique_ptr<mega::MegaNode> node(
        mega_->getNodeByPath(item->id().c_str()));
    mega_->getThumbnail(node.get(), cache.c_str(), &listener);
    semaphore.wait();
    if (r->is_cancelled())
      while (listener.status_ == Listener::IN_PROGRESS) semaphore.wait();
    mega_->removeRequestListener(&listener);
    if (listener.status_ == Listener::SUCCESS) {
      std::fstream cache_file(cache.c_str(),
                              std::fstream::in | std::fstream::binary);
      if (!cache_file) {
        callback->error({500, "Couldn't open cache file " + cache});
        return Error{500, "couldn't open cache file " + cache};
      }
      std::array<char, BUFFER_SIZE> buffer;
      do {
        cache_file.read(buffer.data(), BUFFER_SIZE);
        callback->receivedData(buffer.data(), cache_file.gcount());
      } while (cache_file.gcount() > 0);
      callback->done();
    } else {
      cloudstorage::DownloadFileRequest::generateThumbnail(r, item, callback);
    }
    std::remove(cache.c_str());
    return nullptr;
  });
  return std::move(r);
}

ICloudProvider::DeleteItemRequest::Pointer MegaNz::deleteItemAsync(
    IItem::Pointer item, DeleteItemCallback callback) {
  auto r = util::make_unique<Request<EitherError<void>>>(shared_from_this());
  r->set_resolver([this, item, callback](
                      Request<EitherError<void>>* r) -> EitherError<void> {
    if (!ensureAuthorized(r)) {
      Error e{401, "authrization failed"};
      callback(e);
      return e;
    }
    std::unique_ptr<mega::MegaNode> node(
        mega_->getNodeByPath(item->id().c_str()));
    Request<EitherError<void>>::Semaphore semaphore(r);
    RequestListener listener(&semaphore);
    mega_->remove(node.get(), &listener);
    semaphore.wait();
    mega_->removeRequestListener(&listener);
    if (listener.status_ == Listener::SUCCESS) {
      callback(nullptr);
      return nullptr;
    } else {
      Error e{500, ""};
      callback(e);
      return e;
    }
  });
  return std::move(r);
}

ICloudProvider::CreateDirectoryRequest::Pointer MegaNz::createDirectoryAsync(
    IItem::Pointer parent, const std::string& name,
    CreateDirectoryCallback callback) {
  auto r = util::make_unique<Request<EitherError<IItem>>>(shared_from_this());
  r->set_resolver([=](Request<EitherError<IItem>>* r) -> EitherError<IItem> {
    if (!ensureAuthorized(r)) {
      Error e{401, "authorization error"};
      callback(e);
      return e;
    }
    std::unique_ptr<mega::MegaNode> parent_node(
        mega_->getNodeByPath(parent->id().c_str()));
    if (!parent_node) {
      Error e{404, "parent not found"};
      callback(e);
      return e;
    }
    Request<EitherError<IItem>>::Semaphore semaphore(r);
    RequestListener listener(&semaphore);
    mega_->createFolder(name.c_str(), parent_node.get(), &listener);
    semaphore.wait();
    mega_->removeRequestListener(&listener);
    if (listener.status_ == Listener::SUCCESS) {
      std::unique_ptr<mega::MegaNode> node(
          mega_->getNodeByHandle(listener.node_));
      auto item = toItem(node.get());
      callback(item);
      return item;
    } else {
      Error e{500, "couldn't create folder"};
      callback(e);
      return e;
    }
  });
  return std::move(r);
}

ICloudProvider::MoveItemRequest::Pointer MegaNz::moveItemAsync(
    IItem::Pointer source, IItem::Pointer destination,
    MoveItemCallback callback) {
  auto r = util::make_unique<Request<EitherError<void>>>(shared_from_this());
  r->set_resolver([=](Request<EitherError<void>>* r) -> EitherError<void> {
    if (!ensureAuthorized(r)) {
      Error error{401, "authorization error"};
      callback(error);
      return error;
    }
    std::unique_ptr<mega::MegaNode> source_node(
        mega_->getNodeByPath(source->id().c_str()));
    std::unique_ptr<mega::MegaNode> destination_node(
        mega_->getNodeByPath(destination->id().c_str()));
    if (source_node && destination_node) {
      Request<EitherError<void>>::Semaphore semaphore(r);
      RequestListener listener(&semaphore);
      mega_->moveNode(source_node.get(), destination_node.get(), &listener);
      semaphore.wait();
      mega_->removeRequestListener(&listener);
      if (listener.status_ == Listener::SUCCESS) {
        callback(nullptr);
        return nullptr;
      }
    }
    Error error{500, ""};
    callback(error);
    return error;
  });
  return std::move(r);
}

ICloudProvider::RenameItemRequest::Pointer MegaNz::renameItemAsync(
    IItem::Pointer item, const std::string& name, RenameItemCallback callback) {
  auto r = util::make_unique<Request<EitherError<void>>>(shared_from_this());
  r->set_resolver([=](Request<EitherError<void>>* r) -> EitherError<void> {
    if (!ensureAuthorized(r)) {
      Error error{401, "authorization error"};
      callback(error);
      return error;
    }
    std::unique_ptr<mega::MegaNode> node(
        mega_->getNodeByPath(item->id().c_str()));
    if (node) {
      Request<EitherError<void>>::Semaphore semaphore(r);
      RequestListener listener(&semaphore);
      mega_->renameNode(node.get(), name.c_str(), &listener);
      semaphore.wait();
      mega_->removeRequestListener(&listener);
      if (listener.status_ == Listener::SUCCESS) {
        callback(nullptr);
        return nullptr;
      }
    }
    callback(Error{500, ""});
    return Error{500, ""};
  });
  return std::move(r);
}

std::function<EitherError<void>(Request<EitherError<void>>*)>
MegaNz::downloadResolver(IItem::Pointer item,
                         IDownloadFileCallback::Pointer callback, int64_t start,
                         int64_t size) {
  return [this, item, callback, start,
          size](Request<EitherError<void>>* r) -> EitherError<void> {
    if (!ensureAuthorized(r)) {
      Error e = {401, "authorization failed"};
      if (!r->is_cancelled())
        callback->error({401, "Authorization failed."});
      else
        e = {IHttpRequest::Aborted, ""};
      return e;
    }
    std::unique_ptr<mega::MegaNode> node(
        mega_->getNodeByPath(item->id().c_str()));
    Request<EitherError<void>>::Semaphore semaphore(r);
    TransferListener listener(&semaphore);
    listener.download_callback_ = callback;
    listener.request_ = r;
    mega_->startStreaming(node.get(), start,
                          size == -1 ? node->getSize() - start : size,
                          &listener);
    semaphore.wait();
    if (r->is_cancelled())
      while (listener.status_ == Listener::IN_PROGRESS) semaphore.wait();
    mega_->removeTransferListener(&listener);
    if (listener.status_ != Listener::SUCCESS) {
      if (!r->is_cancelled()) {
        callback->error({500, "Failed to download"});
        return Error{500, "failed to download"};
      } else
        return Error{IHttpRequest::Aborted, ""};
    } else {
      callback->done();
      return nullptr;
    }
  };
}

bool MegaNz::login(Request<EitherError<void>>* r) {
  Authorize::Semaphore semaphore(r);
  RequestListener auth_listener(&semaphore);
  auto data = credentialsFromString(token());
  std::string mail = data.first;
  std::string private_key = data.second;
  std::unique_ptr<char[]> hash(
      mega_->getStringHash(private_key.c_str(), mail.c_str()));
  mega_->fastLogin(mail.c_str(), hash.get(), private_key.c_str(),
                   &auth_listener);
  semaphore.wait();
  mega_->removeRequestListener(&auth_listener);
  return auth_listener.status_ == Listener::SUCCESS;
}

std::string MegaNz::passwordHash(const std::string& password) const {
  std::unique_ptr<char[]> hash(mega_->getBase64PwKey(password.c_str()));
  return std::string(hash.get());
}

IItem::Pointer MegaNz::toItem(MegaNode* node) {
  std::unique_ptr<char[]> path(mega_->getNodePath(node));
  auto item = util::make_unique<Item>(
      node->getName(), path.get(),
      node->isFolder() ? IItem::FileType::Directory : IItem::FileType::Unknown);
  std::unique_ptr<char[]> handle(node->getBase64Handle());
  item->set_url(endpoint() + "/?file=" + handle.get() +
                "&state=" + auth()->state());
  return std::move(item);
}

std::string MegaNz::randomString(int length) {
  std::unique_lock<std::mutex> lock(mutex_);
  std::uniform_int_distribution<char> dist('a', 'z');
  std::string result;
  for (int i = 0; i < length; i++) result += dist(engine_);
  return result;
}

std::string MegaNz::temporaryFileName() {
  return temporary_directory_ + randomString(CACHE_FILENAME_LENGTH);
}

template <class T>
bool MegaNz::ensureAuthorized(Request<T>* r) {
  if (!authorized_)
    return r->reauthorize();
  else
    return true;
}

IAuth::Token::Pointer MegaNz::authorizationCodeToToken(
    const std::string& code) const {
  auto data = credentialsFromString(code);
  IAuth::Token::Pointer token = util::make_unique<IAuth::Token>();
  token->token_ = data.first + Auth::SEPARATOR + passwordHash(data.second);
  token->refresh_token_ = token->token_;
  return token;
}

MegaNz::Auth::Auth() {}

std::string MegaNz::Auth::authorizeLibraryUrl() const {
  return redirect_uri() + "/login?state=" + state();
}

IHttpRequest::Pointer MegaNz::Auth::exchangeAuthorizationCodeRequest(
    std::ostream&) const {
  return nullptr;
}

IHttpRequest::Pointer MegaNz::Auth::refreshTokenRequest(std::ostream&) const {
  return nullptr;
}

IAuth::Token::Pointer MegaNz::Auth::exchangeAuthorizationCodeResponse(
    std::istream&) const {
  return nullptr;
}

IAuth::Token::Pointer MegaNz::Auth::refreshTokenResponse(std::istream&) const {
  return nullptr;
}

MegaNz::Authorize::~Authorize() { cancel(); }

}  // namespace cloudstorage
