#include "cardreader.h"
#include "common.h"

// CardReader implementation
Napi::Object CardReader::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "CardReader", {
        InstanceMethod("get_status", &CardReader::GetStatus),
        InstanceMethod("_connect", &CardReader::Connect),
        InstanceMethod("_disconnect", &CardReader::Disconnect),
        InstanceMethod("_transmit", &CardReader::Transmit),
        InstanceMethod("_control", &CardReader::Control),
        InstanceMethod("close", &CardReader::Close),

        // Constants: Share Mode
        InstanceValue("SCARD_SHARE_SHARED", Napi::Number::New(env, SCARD_SHARE_SHARED)),
        InstanceValue("SCARD_SHARE_EXCLUSIVE", Napi::Number::New(env, SCARD_SHARE_EXCLUSIVE)),
        InstanceValue("SCARD_SHARE_DIRECT", Napi::Number::New(env, SCARD_SHARE_DIRECT)),
        
        // Control Code
        InstanceValue("IOCTL_CCID_ESCAPE", Napi::Number::New(env, IOCTL_CCID_ESCAPE)),
        
        // Protocol
        InstanceValue("SCARD_PROTOCOL_T0", Napi::Number::New(env, SCARD_PROTOCOL_T0)),
        InstanceValue("SCARD_PROTOCOL_T1", Napi::Number::New(env, SCARD_PROTOCOL_T1)),
        InstanceValue("SCARD_PROTOCOL_RAW", Napi::Number::New(env, SCARD_PROTOCOL_RAW)),
        
        // State
        InstanceValue("SCARD_STATE_UNAWARE", Napi::Number::New(env, SCARD_STATE_UNAWARE)),
        InstanceValue("SCARD_STATE_IGNORE", Napi::Number::New(env, SCARD_STATE_IGNORE)),
        InstanceValue("SCARD_STATE_CHANGED", Napi::Number::New(env, SCARD_STATE_CHANGED)),
        InstanceValue("SCARD_STATE_UNKNOWN", Napi::Number::New(env, SCARD_STATE_UNKNOWN)),
        InstanceValue("SCARD_STATE_UNAVAILABLE", Napi::Number::New(env, SCARD_STATE_UNAVAILABLE)),
        InstanceValue("SCARD_STATE_EMPTY", Napi::Number::New(env, SCARD_STATE_EMPTY)),
        InstanceValue("SCARD_STATE_PRESENT", Napi::Number::New(env, SCARD_STATE_PRESENT)),
        InstanceValue("SCARD_STATE_ATRMATCH", Napi::Number::New(env, SCARD_STATE_ATRMATCH)),
        InstanceValue("SCARD_STATE_EXCLUSIVE", Napi::Number::New(env, SCARD_STATE_EXCLUSIVE)),
        InstanceValue("SCARD_STATE_INUSE", Napi::Number::New(env, SCARD_STATE_INUSE)),
        InstanceValue("SCARD_STATE_MUTE", Napi::Number::New(env, SCARD_STATE_MUTE)),
        
        // Disconnect disposition
        InstanceValue("SCARD_LEAVE_CARD", Napi::Number::New(env, SCARD_LEAVE_CARD)),
        InstanceValue("SCARD_RESET_CARD", Napi::Number::New(env, SCARD_RESET_CARD)),
        InstanceValue("SCARD_UNPOWER_CARD", Napi::Number::New(env, SCARD_UNPOWER_CARD)),
        InstanceValue("SCARD_EJECT_CARD", Napi::Number::New(env, SCARD_EJECT_CARD))
    });

    Napi::FunctionReference* constructor = new Napi::FunctionReference();
    *constructor = Napi::Persistent(func);
    env.SetInstanceData(constructor);

    exports.Set("CardReader", func);
    return exports;
}

CardReader::CardReader(const Napi::CallbackInfo& info) 
    : Napi::ObjectWrap<CardReader>(info),
      m_card_context(0),
      m_status_card_context(0),
      m_card_handle(0),
      m_mutex(),
      m_cond(),
      m_state(0) {
    
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(info.Env(), "Reader name expected").ThrowAsJavaScriptException();
        return;
    }

    m_name = info[0].As<Napi::String>().Utf8Value();

    // Set properties on the JavaScript object
    Napi::Object jsThis = info.This().As<Napi::Object>();
    jsThis.Set("name", info[0]);
    jsThis.Set("connected", Napi::Boolean::New(info.Env(), false));
}

CardReader::~CardReader() {
    if (m_status_thread.joinable()) {
        SCardCancel(m_status_card_context);
        m_status_thread.join();
    }

    if (m_card_context) {
        SCardReleaseContext(m_card_context);
    }
}

// StatusWorker implementation
CardReader::StatusWorker::StatusWorker(Napi::Function& callback, CardReader* reader)
    : Napi::AsyncWorker(callback),
      reader_(reader) {
    async_result_ = new AsyncResult();
}

CardReader::StatusWorker::~StatusWorker() {
    delete async_result_;
}

void CardReader::StatusWorker::Execute() {
    // Will run in worker thread
    // Implementation omitted for brevity
}

void CardReader::StatusWorker::OnOK() {
    Napi::HandleScope scope(Env());
    
    Napi::Object status = Napi::Object::New(Env());
    status.Set("state", Napi::Number::New(Env(), async_result_->status));
    
    if (async_result_->atrlen > 0) {
        status.Set("atr", Napi::Buffer<uint8_t>::Copy(Env(), 
                                                     async_result_->atr, 
                                                     async_result_->atrlen));
    }
    
    Callback().Call({Env().Undefined(), status});
}

void CardReader::StatusWorker::NotifyJS() {
    // Implementation omitted for brevity
}

// ConnectWorker implementation
CardReader::ConnectWorker::ConnectWorker(Napi::Function& callback, CardReader* reader, ConnectInput* input)
    : Napi::AsyncWorker(callback),
      reader_(reader),
      input_(input) {
}

CardReader::ConnectWorker::~ConnectWorker() {
    delete input_;
}

void CardReader::ConnectWorker::Execute() {
    LONG result = SCARD_S_SUCCESS;
    
    // Lock mutex
    std::unique_lock<std::mutex> lock(reader_->m_mutex);
    
    // Is context established
    if (!reader_->m_card_context) {
        result = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &reader_->m_card_context);
    }
    
    // Connect
    if (result == SCARD_S_SUCCESS) {
        result = SCardConnect(reader_->m_card_context,
                             reader_->m_name.c_str(),
                             input_->share_mode,
                             input_->pref_protocol,
                             &reader_->m_card_handle,
                             &result_.card_protocol);
    }
    
    result_.result = result;
    
    if (result != SCARD_S_SUCCESS) {
        SetError(error_msg("SCardConnect", result));
    }
}

void CardReader::ConnectWorker::OnOK() {
    Napi::HandleScope scope(Env());
    
    // Set connected to true on the JavaScript object
    Napi::Object jsThis = reader_->Value().As<Napi::Object>();
    jsThis.Set("connected", Napi::Boolean::New(Env(), true));
    
    Callback().Call({
        Env().Undefined(),
        Napi::Number::New(Env(), result_.card_protocol)
    });
}

// DisconnectWorker implementation 
CardReader::DisconnectWorker::DisconnectWorker(Napi::Function& callback, CardReader* reader, DWORD disposition)
    : Napi::AsyncWorker(callback),
      reader_(reader),
      disposition_(disposition) {
}

CardReader::DisconnectWorker::~DisconnectWorker() {
}

void CardReader::DisconnectWorker::Execute() {
    LONG result = SCARD_S_SUCCESS;
    
    // Lock mutex
    std::unique_lock<std::mutex> lock(reader_->m_mutex);
    
    // Connect
    if (reader_->m_card_handle) {
        result = SCardDisconnect(reader_->m_card_handle, disposition_);
        if (result == SCARD_S_SUCCESS) {
            reader_->m_card_handle = 0;
        }
    }
    
    result_ = result;
    
    if (result != SCARD_S_SUCCESS) {
        SetError(error_msg("SCardDisconnect", result));
    }
}

void CardReader::DisconnectWorker::OnOK() {
    Napi::HandleScope scope(Env());
    
    // Set connected to false on the JavaScript object
    Napi::Object jsThis = reader_->Value().As<Napi::Object>();
    jsThis.Set("connected", Napi::Boolean::New(Env(), false));
    
    Callback().Call({Env().Undefined()});
}

// TransmitWorker implementation
CardReader::TransmitWorker::TransmitWorker(Napi::Function& callback, CardReader* reader, TransmitInput* input)
    : Napi::AsyncWorker(callback),
      reader_(reader),
      input_(input) {
    result_.data = new unsigned char[input_->out_len];
    result_.len = input_->out_len;
}

CardReader::TransmitWorker::~TransmitWorker() {
    delete[] input_->in_data;
    delete input_;
    delete[] result_.data;
}

void CardReader::TransmitWorker::Execute() {
    LONG result = SCARD_E_INVALID_HANDLE;
    
    // Lock mutex
    std::unique_lock<std::mutex> lock(reader_->m_mutex);
    
    // Connected?
    if (reader_->m_card_handle) {
        SCARD_IO_REQUEST send_pci = { input_->card_protocol, sizeof(SCARD_IO_REQUEST) };
        result = SCardTransmit(reader_->m_card_handle, 
                              &send_pci, 
                              input_->in_data, 
                              input_->in_len,
                              NULL, 
                              result_.data, 
                              &result_.len);
    }
    
    result_.result = result;
    
    if (result != SCARD_S_SUCCESS) {
        SetError(error_msg("SCardTransmit", result));
    }
}

void CardReader::TransmitWorker::OnOK() {
    Napi::HandleScope scope(Env());
    
    Callback().Call({
        Env().Undefined(),
        Napi::Buffer<unsigned char>::Copy(Env(), result_.data, result_.len)
    });
}

// ControlWorker implementation
CardReader::ControlWorker::ControlWorker(Napi::Function& callback, CardReader* reader, ControlInput* input)
    : Napi::AsyncWorker(callback),
      reader_(reader),
      input_(input) {
}

CardReader::ControlWorker::~ControlWorker() {
    delete input_;
}

void CardReader::ControlWorker::Execute() {
    LONG result = SCARD_E_INVALID_HANDLE;
    
    // Lock mutex
    std::unique_lock<std::mutex> lock(reader_->m_mutex);
    
    // Connected?
    if (reader_->m_card_handle) {
        result = SCardControl(reader_->m_card_handle,
                             input_->control_code,
                             input_->in_data,
                             input_->in_len,
                             input_->out_data,
                             input_->out_len,
                             &result_.len);
    }
    
    result_.result = result;
    
    if (result != SCARD_S_SUCCESS) {
        SetError(error_msg("SCardControl", result));
    }
}

void CardReader::ControlWorker::OnOK() {
    Napi::HandleScope scope(Env());
    
    Callback().Call({
        Env().Undefined(),
        Napi::Number::New(Env(), result_.len)
    });
}

// CardReader methods
Napi::Value CardReader::GetStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Callback function expected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    Napi::Function callback = info[0].As<Napi::Function>();
    m_status_callback = Napi::Persistent(callback);
    
    // Create thread safe function
    m_tsfn = Napi::ThreadSafeFunction::New(
        env,
        callback,
        "CardReaderStatusCallback",
        0,
        1
    );
    
    // Start the monitoring thread
    m_status_thread = std::thread(HandlerFunction, this);
    
    return env.Undefined();
}

Napi::Value CardReader::Connect(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 3) {
        Napi::TypeError::New(env, "Wrong number of arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    if (!info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsFunction()) {
        Napi::TypeError::New(env, "Wrong arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    ConnectInput* ci = new ConnectInput();
    ci->share_mode = info[0].As<Napi::Number>().Uint32Value();
    ci->pref_protocol = info[1].As<Napi::Number>().Uint32Value();
    Napi::Function callback = info[2].As<Napi::Function>();
    
    // If already connected, just call the callback
    Napi::Object jsThis = info.This().As<Napi::Object>();
    if (jsThis.Get("connected").As<Napi::Boolean>().Value()) {
        callback.Call(info.This(), {env.Undefined()});
        delete ci;
        return env.Undefined();
    }
    
    ConnectWorker* worker = new ConnectWorker(callback, this, ci);
    worker->Queue();
    
    return env.Undefined();
}

Napi::Value CardReader::Disconnect(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 2) {
        Napi::TypeError::New(env, "Wrong number of arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    if (!info[0].IsNumber() || !info[1].IsFunction()) {
        Napi::TypeError::New(env, "Wrong arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    DWORD disposition = info[0].As<Napi::Number>().Uint32Value();
    Napi::Function callback = info[1].As<Napi::Function>();
    
    // If not connected, just call the callback
    Napi::Object jsThis = info.This().As<Napi::Object>();
    if (!jsThis.Get("connected").As<Napi::Boolean>().Value()) {
        callback.Call(info.This(), {env.Undefined()});
        return env.Undefined();
    }
    
    DisconnectWorker* worker = new DisconnectWorker(callback, this, disposition);
    worker->Queue();
    
    return env.Undefined();
}

Napi::Value CardReader::Transmit(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 4) {
        Napi::TypeError::New(env, "Wrong number of arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    if (!info[0].IsBuffer() || !info[1].IsNumber() || !info[2].IsNumber() || !info[3].IsFunction()) {
        Napi::TypeError::New(env, "Wrong arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    // Check if connected
    Napi::Object jsThis = info.This().As<Napi::Object>();
    if (!jsThis.Get("connected").As<Napi::Boolean>().Value()) {
        Napi::Error::New(env, "Card Reader not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    Napi::Buffer<uint8_t> buffer = info[0].As<Napi::Buffer<uint8_t>>();
    uint32_t out_len = info[1].As<Napi::Number>().Uint32Value();
    uint32_t protocol = info[2].As<Napi::Number>().Uint32Value();
    Napi::Function callback = info[3].As<Napi::Function>();
    
    TransmitInput* ti = new TransmitInput();
    ti->card_protocol = protocol;
    ti->in_len = buffer.Length();
    ti->in_data = new unsigned char[ti->in_len];
    memcpy(ti->in_data, buffer.Data(), ti->in_len);
    ti->out_len = out_len;
    
    TransmitWorker* worker = new TransmitWorker(callback, this, ti);
    worker->Queue();
    
    return env.Undefined();
}

Napi::Value CardReader::Control(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 4) {
        Napi::TypeError::New(env, "Wrong number of arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    if (!info[0].IsBuffer() || !info[1].IsNumber() || !info[2].IsBuffer() || !info[3].IsFunction()) {
        Napi::TypeError::New(env, "Wrong arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    // Check if connected
    Napi::Object jsThis = info.This().As<Napi::Object>();
    if (!jsThis.Get("connected").As<Napi::Boolean>().Value()) {
        Napi::Error::New(env, "Card Reader not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    Napi::Buffer<uint8_t> in_buf = info[0].As<Napi::Buffer<uint8_t>>();
    uint32_t control_code = info[1].As<Napi::Number>().Uint32Value();
    Napi::Buffer<uint8_t> out_buf = info[2].As<Napi::Buffer<uint8_t>>();
    Napi::Function callback = info[3].As<Napi::Function>();
    
    ControlInput* ci = new ControlInput();
    ci->control_code = control_code;
    ci->in_data = in_buf.Data();
    ci->in_len = in_buf.Length();
    ci->out_data = out_buf.Data();
    ci->out_len = out_buf.Length();
    
    ControlWorker* worker = new ControlWorker(callback, this, ci);
    worker->Queue();
    
    return env.Undefined();
}

Napi::Value CardReader::Close(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    LONG result = SCARD_S_SUCCESS;
    
    if (m_status_thread.joinable()) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_state == 0) {
            int ret;
            int times = 0;
            m_state = 1;
            do {
                result = SCardCancel(m_status_card_context);
                ret = std::cv_status::timeout == m_cond.wait_for(lock, std::chrono::microseconds(10000000)) ? -1 : 0;
            } while ((ret != 0) && (++times < 5));
        }
        
        lock.unlock();
        m_status_thread.join();
        m_status_thread = std::thread();
    }
    
    // Release ThreadSafeFunction if it's active
    if (m_tsfn) {
        m_tsfn.Release();
    }
    
    return Napi::Number::New(env, result);
}

void CardReader::HandlerFunction(void* arg) {
    CardReader* reader = static_cast<CardReader*>(arg);
    AsyncResult* async_result = new AsyncResult();
    
    LONG result = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &reader->m_status_card_context);
    
    SCARD_READERSTATE card_reader_state = SCARD_READERSTATE();
    card_reader_state.szReader = reader->m_name.c_str();
    card_reader_state.dwCurrentState = SCARD_STATE_UNAWARE;
    
    auto callback = [reader, &card_reader_state, async_result](Napi::Env env, Napi::Function jsCallback) {
        if (reader->m_state == 1) {
            // Exit requested by user
            reader->m_cond.notify_all();
        } else {
            Napi::Object status = Napi::Object::New(env);
            status.Set("state", Napi::Number::New(env, async_result->status));
            
            if (async_result->atrlen > 0) {
                status.Set("atr", Napi::Buffer<uint8_t>::Copy(env, 
                                                             async_result->atr, 
                                                             async_result->atrlen));
            }
            
            jsCallback.Call({env.Undefined(), status});
        }
    };
    
    while (!reader->m_state) {
        result = SCardGetStatusChange(reader->m_status_card_context, INFINITE, &card_reader_state, 1);
        
        std::unique_lock<std::mutex> lock(reader->m_mutex);
        if (reader->m_state == 1) {
            // Exit requested by user
            reader->m_cond.notify_all();
        } else if (result != (LONG)SCARD_S_SUCCESS) {
            // Exit this loop due to errors
            reader->m_state = 2;
        }
        
        async_result->do_exit = (reader->m_state != 0);
        async_result->result = result;
        if (card_reader_state.dwEventState == card_reader_state.dwCurrentState) {
            async_result->status = 0;
        } else {
            async_result->status = card_reader_state.dwEventState;
        }
        memcpy(async_result->atr, card_reader_state.rgbAtr, card_reader_state.cbAtr);
        async_result->atrlen = card_reader_state.cbAtr;
        
        lock.unlock();
        
        reader->m_tsfn.BlockingCall(callback);
        card_reader_state.dwCurrentState = card_reader_state.dwEventState;
    }
    
    // Final cleanup
    reader->m_tsfn.Release();
    delete async_result;
}