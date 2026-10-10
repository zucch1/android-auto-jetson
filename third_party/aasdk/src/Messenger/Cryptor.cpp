// This file is part of aasdk library project.
// Copyright (C) 2018 f1x.studio (Michal Szwaj)
// Copyright (C) 2024 CubeOne (Simon Dean - simon.dean@cubeone.co.uk)
//
// aasdk is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// aasdk is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with aasdk. If not, see <http://www.gnu.org/licenses/>.

#include <algorithm>
#include <functional>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Error/Error.hpp>
#include <aasdk/Common/Log.hpp>
#include <aasdk/Common/ModernLogger.hpp>
#include <aa/tls/Credentials.hpp>

namespace aasdk {
  namespace messenger {

    Cryptor::Cryptor(transport::ISSLWrapper::Pointer sslWrapper)
        : sslWrapper_(std::move(sslWrapper)), maxBufferSize_(1024 * 20), certificate_(nullptr), privateKey_(nullptr),
          context_(nullptr), ssl_(nullptr), isActive_(false) {

    }

    void Cryptor::init() {
      std::lock_guard<decltype(mutex_)> lock(mutex_);

      // Downstream sanitization 2026-10-10 (android-auto-jetson): the embedded
      // upstream head-unit credential was removed from this distribution; see
      // PROVENANCE.md and THIRD_PARTY_NOTICES.md. Credential acquisition is
      // fail-closed and HU_KEY_PATH-only through aa::tls::load_credentials();
      // there is no embedded fallback and no bundled credential files.
      auto credentials = [&] {
        try {
          return aa::tls::load_credentials();
        } catch (const aa::tls::Error& failure) {
          AASDK_LOG(error) << failure.what();
          throw;
        }
      }();
      certificate_ = credentials.certificate.release();
      privateKey_ = credentials.private_key.release();

      auto method = sslWrapper_->getMethod();

      if (method == nullptr) {
        throw error::Error(error::ErrorCode::SSL_METHOD);
      }

      context_ = sslWrapper_->createContext(method);

      if (context_ == nullptr) {
        throw error::Error(error::ErrorCode::SSL_CONTEXT_CREATION);
      }

      if (!sslWrapper_->useCertificate(context_, certificate_)) {
        throw error::Error(error::ErrorCode::SSL_USE_CERTIFICATE);
      }

      if (!sslWrapper_->usePrivateKey(context_, privateKey_)) {
        throw error::Error(error::ErrorCode::SSL_USE_PRIVATE_KEY);
      }

      ssl_ = sslWrapper_->createInstance(context_);

      if (ssl_ == nullptr) {
        throw error::Error(error::ErrorCode::SSL_HANDLER_CREATION);
      }

      bIOs_ = sslWrapper_->createBIOs();

      if (bIOs_.first == nullptr) {
        throw error::Error(error::ErrorCode::SSL_READ_BIO_CREATION);
      }

      if (bIOs_.second == nullptr) {
        throw error::Error(error::ErrorCode::SSL_WRITE_BIO_CREATION);
      }

      sslWrapper_->setBIOs(ssl_, bIOs_, maxBufferSize_);

      sslWrapper_->setConnectState(ssl_);
    }

    void Cryptor::deinit() {
      std::lock_guard<decltype(mutex_)> lock(mutex_);

      if (ssl_ != nullptr) {
        sslWrapper_->free(ssl_);
        ssl_ = nullptr;
      }

      bIOs_ = std::make_pair(nullptr, nullptr);

      if (context_ != nullptr) {
        sslWrapper_->free(context_);
        context_ = nullptr;
      }

      if (certificate_ != nullptr) {
        sslWrapper_->free(certificate_);
        certificate_ = nullptr;
      }

      if (privateKey_ != nullptr) {
        sslWrapper_->free(privateKey_);
        privateKey_ = nullptr;
      }
    }

    bool Cryptor::doHandshake() {
      std::lock_guard<decltype(mutex_)> lock(mutex_);

      auto result = sslWrapper_->doHandshake(ssl_);
      if (result == SSL_ERROR_WANT_READ) {
        return false;
      } else if (result == SSL_ERROR_NONE) {
        isActive_ = true;
        return true;
      } else {
        throw error::Error(error::ErrorCode::SSL_HANDSHAKE, result);
      }
    }

    size_t Cryptor::encrypt(common::Data &output, const common::DataConstBuffer &buffer) {
      std::lock_guard<decltype(mutex_)> lock(mutex_);

      size_t totalWrittenBytes = 0;

      while (totalWrittenBytes < buffer.size) {
        const common::DataConstBuffer currentBuffer(buffer.cdata, buffer.size, totalWrittenBytes);
        const auto writeSize = sslWrapper_->sslWrite(ssl_, currentBuffer.cdata, currentBuffer.size);

        if (writeSize <= 0) {
          throw error::Error(error::ErrorCode::SSL_WRITE, sslWrapper_->getError(ssl_, writeSize));
        }

        totalWrittenBytes += writeSize;
      }

      return this->read(output);
    }

    size_t Cryptor::decrypt(common::Data &output, const common::DataConstBuffer &buffer, int frameLength) {
      int overhead = 29;
      int length = frameLength - overhead;
      std::lock_guard<decltype(mutex_)> lock(mutex_);

      this->write(buffer);
      const size_t beginOffset = output.size();

      size_t totalReadSize = 0;                                                                               // Initialise
      size_t availableBytes = length;
      size_t readBytes = (availableBytes - totalReadSize) > 2048 ? 2048 : availableBytes -
                                                                  totalReadSize;                     // Calculate How many Bytes to Read
      output.resize(output.size() +
                    readBytes);                                                               // Resize Output to match the bytes we want to read

      // We try to be a bit more explicit here, using the frame length from the frame itself rather than just blindly reading from the SSL buffer.

      while (readBytes > 0) {
        const auto &currentBuffer = common::DataBuffer(output, totalReadSize + beginOffset);
        auto readSize = sslWrapper_->sslRead(ssl_, currentBuffer.data, currentBuffer.size);

        if (readSize <= 0) {
          const auto nativeError = sslWrapper_->getError(ssl_, readSize);
          const std::string info = "decrypt sslRead<=0"
                                   " frameLength=" + std::to_string(frameLength) +
                                   " payloadLength=" + std::to_string(length) +
                                   " availableBytes=" + std::to_string(availableBytes) +
                                   " totalReadSize=" + std::to_string(totalReadSize) +
                                   " requestedReadBytes=" + std::to_string(readBytes) +
                                   " returnCode=" + std::to_string(readSize);
          throw error::Error(error::ErrorCode::SSL_READ, nativeError, info);
        }

        totalReadSize += readSize;
        availableBytes = sslWrapper_->getAvailableBytes(ssl_);
        readBytes = (length - totalReadSize) > 2048 ? 2048 : length - totalReadSize;
        output.resize(output.size() + readBytes);
      }

      return totalReadSize;
    }

    common::Data Cryptor::readHandshakeBuffer() {
      std::lock_guard<decltype(mutex_)> lock(mutex_);

      common::Data output;
      this->read(output);
      return output;
    }

    void Cryptor::writeHandshakeBuffer(const common::DataConstBuffer &buffer) {
      std::lock_guard<decltype(mutex_)> lock(mutex_);

      this->write(buffer);
    }

    size_t Cryptor::read(common::Data &output) {
      const auto pendingSize = sslWrapper_->bioCtrlPending(bIOs_.second);

      size_t beginOffset = output.size();
      output.resize(beginOffset + pendingSize);
      size_t totalReadSize = 0;

      while (totalReadSize < pendingSize) {
        const auto &currentBuffer = common::DataBuffer(output, totalReadSize + beginOffset);
        const auto readSize = sslWrapper_->bioRead(bIOs_.second, currentBuffer.data, currentBuffer.size);

        if (readSize <= 0) {
          const auto nativeError = sslWrapper_->getError(ssl_, readSize);
          const std::string info = "read bioRead<=0"
                                   " pendingSize=" + std::to_string(pendingSize) +
                                   " totalReadSize=" + std::to_string(totalReadSize) +
                                   " currentBufferSize=" + std::to_string(currentBuffer.size) +
                                   " returnCode=" + std::to_string(readSize);
          throw error::Error(error::ErrorCode::SSL_BIO_READ, nativeError, info);
        }

        totalReadSize += readSize;
      }

      return totalReadSize;
    }

    void Cryptor::write(const common::DataConstBuffer &buffer) {
      size_t totalWrittenBytes = 0;

      while (totalWrittenBytes < buffer.size) {
        const common::DataConstBuffer currentBuffer(buffer.cdata, buffer.size, totalWrittenBytes);
        const auto writeSize = sslWrapper_->bioWrite(bIOs_.first, currentBuffer.cdata, currentBuffer.size);

        if (writeSize <= 0) {
          throw error::Error(error::ErrorCode::SSL_BIO_WRITE, sslWrapper_->getError(ssl_, writeSize));
        }

        totalWrittenBytes += writeSize;
      }
    }

    bool Cryptor::isActive() const {
      std::lock_guard<decltype(mutex_)> lock(mutex_);

      return isActive_;
    }


  }
}
