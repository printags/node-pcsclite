#ifndef CARDREADER_H
#define CARDREADER_H

#include <napi.h>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#ifdef __APPLE__
#include <PCSC/winscard.h>
#include <PCSC/wintypes.h>
#else
#include <winscard.h>
#endif

#ifdef _WIN32
#define MAX_ATR_SIZE 33
#endif
#ifdef WIN32
#define IOCTL_CCID_ESCAPE (0x42000000 + 3500)
#else
#define IOCTL_CCID_ESCAPE (0x42000000 + 1)
#endif

class CardReader : public Napi::ObjectWrap<CardReader> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    CardReader(const Napi::CallbackInfo& info);
    ~CardReader();

    const SCARDHANDLE& GetHandler() const { return m_card_handle; };

private:
    // Structures
    struct ConnectInput {
        DWORD share_mode;
        DWORD pref_protocol;
    };

    struct ConnectResult {
        LONG result;
        DWORD card_protocol;
    };

    struct TransmitInput {
        DWORD card_protocol;
        LPBYTE in_data;
        DWORD in_len;
        DWORD out_len;
    };

    struct TransmitResult {
        LONG result;
        LPBYTE data;
        DWORD len;
    };

    struct ControlInput {
        DWORD control_code;
        LPCVOID in_data;
        DWORD in_len;
        LPVOID out_data;
        DWORD out_len;
    };

    struct ControlResult {
        LONG result;
        DWORD len;
    };

    struct AsyncResult {
        LONG result;
        DWORD status;
        BYTE atr[MAX_ATR_SIZE];
        DWORD atrlen;
        bool do_exit;
    };

    // AsyncWorker classes
    class ConnectWorker : public Napi::AsyncWorker {
    public:
        ConnectWorker(Napi::Function& callback, CardReader* reader, ConnectInput* input);
        ~ConnectWorker();
        void Execute() override;
        void OnOK() override;
    private:
        CardReader* reader_;
        ConnectInput* input_;
        ConnectResult result_;
    };

    class DisconnectWorker : public Napi::AsyncWorker {
    public:
        DisconnectWorker(Napi::Function& callback, CardReader* reader, DWORD disposition);
        ~DisconnectWorker();
        void Execute() override;
        void OnOK() override;
    private:
        CardReader* reader_;
        DWORD disposition_;
        LONG result_;
    };

    class TransmitWorker : public Napi::AsyncWorker {
    public:
        TransmitWorker(Napi::Function& callback, CardReader* reader, TransmitInput* input);
        ~TransmitWorker();
        void Execute() override;
        void OnOK() override;
    private:
        CardReader* reader_;
        TransmitInput* input_;
        TransmitResult result_;
    };

    class ControlWorker : public Napi::AsyncWorker {
    public:
        ControlWorker(Napi::Function& callback, CardReader* reader, ControlInput* input);
        ~ControlWorker();
        void Execute() override;
        void OnOK() override;
    private:
        CardReader* reader_;
        ControlInput* input_;
        ControlResult result_;
    };

    class StatusWorker : public Napi::AsyncWorker {
    public:
        StatusWorker(Napi::Function& callback, CardReader* reader);
        ~StatusWorker();
        void Execute() override;
        void OnOK() override;
        void NotifyJS();
    private:
        CardReader* reader_;
        AsyncResult* async_result_;
    };

    // Napi methods
    Napi::Value GetStatus(const Napi::CallbackInfo& info);
    Napi::Value Connect(const Napi::CallbackInfo& info);
    Napi::Value Disconnect(const Napi::CallbackInfo& info);
    Napi::Value Transmit(const Napi::CallbackInfo& info);
    Napi::Value Control(const Napi::CallbackInfo& info);
    Napi::Value Close(const Napi::CallbackInfo& info);

    // Thread function
    static void HandlerFunction(void* arg);

    // Member variables
    SCARDCONTEXT m_card_context;
    SCARDCONTEXT m_status_card_context;
    SCARDHANDLE m_card_handle;
    std::string m_name;
    std::thread m_status_thread;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    int m_state;
    Napi::ThreadSafeFunction m_tsfn;
    Napi::FunctionReference m_status_callback;
};

#endif /* CARDREADER_H */