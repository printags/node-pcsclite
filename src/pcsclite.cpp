#include "pcsclite.h"
#include "common.h"

// PCSCLite implementation

Napi::Object PCSCLite::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "PCSCLite", {
        InstanceMethod("start", &PCSCLite::Start),
        InstanceMethod("close", &PCSCLite::Close)
    });

    Napi::FunctionReference* constructor = new Napi::FunctionReference();
    *constructor = Napi::Persistent(func);
    env.SetInstanceData(constructor);

    exports.Set("PCSCLite", func);
    return exports;
}

PCSCLite::PCSCLite(const Napi::CallbackInfo& info) 
    : Napi::ObjectWrap<PCSCLite>(info),
      m_card_context(0),
      m_card_reader_state(),
      m_mutex(),
      m_cond(),
      m_pnp(false),
      m_state(0) {
    
    // Windows-specific service initialization code
#ifdef _WIN32
    HKEY hKey;
    DWORD startStatus, datacb = sizeof(DWORD);
    LONG _res;
    _res = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Services\\SCardSvr", 0, KEY_READ, &hKey);
    if (_res != ERROR_SUCCESS) {
        printf("Reg Open Key exited with %d\n", _res);
        goto postServiceCheck;
    }
    _res = RegQueryValueEx(hKey, "Start", NULL, NULL, (LPBYTE)&startStatus, &datacb);
    if (_res != ERROR_SUCCESS) {
        printf("Reg Query Value exited with %d\n", _res);
        goto postServiceCheck;
    }
    if (startStatus != 2) {
        SHELLEXECUTEINFO seInfo = {0};
        seInfo.cbSize = sizeof(SHELLEXECUTEINFO);
        seInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
        seInfo.hwnd = NULL;
        seInfo.lpVerb = "runas";
        seInfo.lpFile = "sc.exe";
        seInfo.lpParameters = "config SCardSvr start=auto";
        seInfo.lpDirectory = NULL;
        seInfo.nShow = SW_SHOWNORMAL;
        seInfo.hInstApp = NULL;
        if (!ShellExecuteEx(&seInfo)) {
            printf("Shell Execute failed with %d\n", GetLastError());
            goto postServiceCheck;
        }
        WaitForSingleObject(seInfo.hProcess, INFINITE);
        CloseHandle(seInfo.hProcess);
    }
postServiceCheck:
#endif // _WIN32

    LONG result;
    do {
        result = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &m_card_context);
    } while(result == SCARD_E_NO_SERVICE || result == SCARD_E_SERVICE_STOPPED);
    
    if (result != SCARD_S_SUCCESS) {
        Napi::Error::New(info.Env(), error_msg("SCardEstablishContext", result)).ThrowAsJavaScriptException();
        return;
    } 
    
    m_card_reader_state.szReader = "\\\\?PnP?\\Notification";
    m_card_reader_state.dwCurrentState = SCARD_STATE_UNAWARE;
    result = SCardGetStatusChange(m_card_context, 0, &m_card_reader_state, 1);

    if ((result != SCARD_S_SUCCESS) && (result != (LONG)SCARD_E_TIMEOUT)) {
        Napi::Error::New(info.Env(), error_msg("SCardGetStatusChange", result)).ThrowAsJavaScriptException();
    } else {
        m_pnp = !(m_card_reader_state.dwEventState & SCARD_STATE_UNKNOWN);
    }
}

PCSCLite::~PCSCLite() {
    if (m_status_thread.joinable()) {
        SCardCancel(m_card_context);
        m_status_thread.join();
    }

    if (m_card_context) {
        SCardReleaseContext(m_card_context);
    }
}

// ReaderWorker implementation
PCSCLite::ReaderWorker::ReaderWorker(Napi::Function& callback, PCSCLite* pcsclite)
    : Napi::AsyncWorker(callback),
      pcsclite_(pcsclite) {
    async_result_ = new AsyncResult();
}

PCSCLite::ReaderWorker::~ReaderWorker() {
    if (async_result_) {
#ifdef SCARD_AUTOALLOCATE
        if (async_result_->readers_name) {
            SCardFreeMemory(pcsclite_->m_card_context, async_result_->readers_name);
        }
#else
        delete[] async_result_->readers_name;
#endif
        delete async_result_;
    }
}

void PCSCLite::ReaderWorker::Execute() {
    // This will run on the worker thread
    LONG result = pcsclite_->get_card_readers(async_result_);
    
    if (result == (LONG)SCARD_E_NO_READERS_AVAILABLE) {
        result = SCARD_S_SUCCESS;
    }

    async_result_->result = result;
    if (result != SCARD_S_SUCCESS) {
        std::string error = error_msg("SCardListReaders", result);
        SetError(error);
        async_result_->err_msg = error;
    }
}

void PCSCLite::ReaderWorker::OnOK() {
    Napi::HandleScope scope(Env());
    
    // Prepare callback arguments
    if (async_result_->readers_name && async_result_->readers_name_length > 0) {
        Callback().Call({
            Env().Undefined(),
            Napi::Buffer<char>::Copy(Env(), 
                                    async_result_->readers_name, 
                                    async_result_->readers_name_length)
        });
    } else {
        Callback().Call({
            Env().Undefined(),
            Env().Undefined()
        });
    }
}

void PCSCLite::ReaderWorker::OnError(const Napi::Error& e) {
    Callback().Call({e.Value()});
}

Napi::Value PCSCLite::Start(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Callback function expected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    Napi::Function callback = info[0].As<Napi::Function>();
    m_callback = Napi::Persistent(callback);
    
    // Create thread safe function
    m_tsfn = Napi::ThreadSafeFunction::New(
        env,
        callback,
        "PCScLiteCallback",
        0,
        1
    );
    
    // Start the monitoring thread
    m_status_thread = std::thread(HandlerFunction, this);
    
    return env.Undefined();
}

Napi::Value PCSCLite::Close(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    LONG result = SCARD_S_SUCCESS;
    
    if (m_pnp) {
        if (m_status_thread.joinable()) {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_state == 0) {
                int ret;
                int times = 0;
                m_state = 1;
                do {
                    result = SCardCancel(m_card_context);
                    ret = std::cv_status::timeout == m_cond.wait_for(lock, std::chrono::microseconds(10000000)) ? -1 : 0;
                } while ((ret != 0) && (++times < 5));
            }
        }
    } else {
        m_state = 1;
    }

    if (m_status_thread.joinable()) {
        m_status_thread.join();
    }
    
    if (m_tsfn) {
        m_tsfn.Release();
    }
    
    return Napi::Number::New(env, result);
}

void PCSCLite::HandlerFunction(void* arg) {
    PCSCLite* pcsclite = static_cast<PCSCLite*>(arg);
    AsyncResult* async_result = new AsyncResult();
    LONG result = SCARD_S_SUCCESS;
    
    auto callback = [pcsclite, async_result](Napi::Env env, Napi::Function jsCallback) {
        if (pcsclite->m_state == 1) {
            // Swallow events: Listening thread was cancelled by user
        } else if ((async_result->result == SCARD_S_SUCCESS) ||
                  (async_result->result == (LONG)SCARD_E_NO_READERS_AVAILABLE)) {
            // Success case
            if (async_result->readers_name && async_result->readers_name_length > 0) {
                jsCallback.Call({
                    env.Undefined(),
                    Napi::Buffer<char>::Copy(env, 
                                           async_result->readers_name, 
                                           async_result->readers_name_length)
                });
            } else {
                jsCallback.Call({
                    env.Undefined(),
                    env.Undefined()
                });
            }
        } else {
            // Error case
            jsCallback.Call({
                Napi::Error::New(env, async_result->err_msg).Value()
            });
        }
        
        // Reset AsyncResult for reuse
#ifdef SCARD_AUTOALLOCATE
        if (async_result->readers_name) {
            SCardFreeMemory(pcsclite->m_card_context, async_result->readers_name);
        }
#else
        delete[] async_result->readers_name;
#endif
        async_result->readers_name = NULL;
        async_result->readers_name_length = 0;
        async_result->result = SCARD_S_SUCCESS;
    };
    
    while (!pcsclite->m_state) {
        // Get card readers
        result = pcsclite->get_card_readers(async_result);
        if (result == (LONG)SCARD_E_NO_READERS_AVAILABLE) {
            result = SCARD_S_SUCCESS;
        }
        
        // Store the result
        async_result->result = result;
        if (result != SCARD_S_SUCCESS) {
            async_result->err_msg = error_msg("SCardListReaders", result);
        }
        
        // Notify the JavaScript thread
        pcsclite->m_tsfn.BlockingCall(callback);
        
        if (result == SCARD_S_SUCCESS) {
            if (pcsclite->m_pnp) {
                // Set current status
                pcsclite->m_card_reader_state.dwCurrentState = pcsclite->m_card_reader_state.dwEventState;
                // Start checking for status change
                result = SCardGetStatusChange(pcsclite->m_card_context,
                                             INFINITE,
                                             &pcsclite->m_card_reader_state,
                                             1);
                
                std::unique_lock<std::mutex> lock(pcsclite->m_mutex);
                async_result->result = result;
                if (pcsclite->m_state) {
                    pcsclite->m_cond.notify_all();
                }
                
                if (result != SCARD_S_SUCCESS) {
                    pcsclite->m_state = 2;
                    async_result->err_msg = error_msg("SCardGetStatusChange", result);
                }
            } else {
                // If PnP is not supported, just wait for 1 second
#ifdef _WIN32
                Sleep(1000);
#else
                usleep(1000000);
#endif
            }
        } else {
            // Error on last card access, stop monitoring
            pcsclite->m_state = 2;
        }
    }
    
    // Final notification before exiting
    async_result->do_exit = true;
    pcsclite->m_tsfn.BlockingCall(callback);
    pcsclite->m_tsfn.Release();
    
    delete async_result;
}

LONG PCSCLite::get_card_readers(AsyncResult* async_result) {
    DWORD readers_name_length;
    LPTSTR readers_name;
    
    LONG result = SCARD_S_SUCCESS;
    
    // Reset the readers_name in the result
    async_result->readers_name = NULL;
    async_result->readers_name_length = 0;
    
#ifdef SCARD_AUTOALLOCATE
    readers_name_length = SCARD_AUTOALLOCATE;
    result = SCardListReaders(m_card_context,
                             NULL,
                             (LPTSTR)&readers_name,
                             &readers_name_length);
#else
    // Find out ReaderNameLength
    result = SCardListReaders(m_card_context,
                             NULL,
                             NULL,
                             &readers_name_length);
    if (result != SCARD_S_SUCCESS) {
        return result;
    }
    
    // Allocate Memory for ReaderName and retrieve all readers in the terminal
    readers_name = new char[readers_name_length];
    result = SCardListReaders(m_card_context,
                             NULL,
                             readers_name,
                             &readers_name_length);
#endif
    
    if (result != SCARD_S_SUCCESS) {
#ifndef SCARD_AUTOALLOCATE
        delete[] readers_name;
#endif
        readers_name = NULL;
        readers_name_length = 0;
        
#ifndef SCARD_AUTOALLOCATE
        // Retry in case of insufficient buffer error
        if (result == (LONG)SCARD_E_INSUFFICIENT_BUFFER) {
            result = get_card_readers(async_result);
        }
#endif
        
        if (result == SCARD_E_NO_SERVICE || result == SCARD_E_SERVICE_STOPPED) {
            SCardReleaseContext(m_card_context);
            SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &m_card_context);
            result = get_card_readers(async_result);
        }
    } else {
        // Store the readers_name in the result
        async_result->readers_name = readers_name;
        async_result->readers_name_length = readers_name_length;
    }
    
    return result;
}