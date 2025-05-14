#ifndef PCSCLITE_H
#define PCSCLITE_H

#include <napi.h>
#ifdef __APPLE__
#include <PCSC/winscard.h>
#include <PCSC/wintypes.h>
#else
#include <winscard.h>
#endif
#include <thread>
#include <mutex>
#include <condition_variable>

class PCSCLite : public Napi::ObjectWrap<PCSCLite> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    PCSCLite(const Napi::CallbackInfo& info);
    ~PCSCLite();

private:
    struct AsyncResult {
        LONG result;
        LPSTR readers_name;
        DWORD readers_name_length;
        bool do_exit;
        std::string err_msg;
    };

    class ReaderWorker : public Napi::AsyncWorker {
    public:
        ReaderWorker(Napi::Function& callback, PCSCLite* pcsclite);
        ~ReaderWorker();

        void Execute() override;
        void OnOK() override;
        void OnError(const Napi::Error& e) override;
        void Notify();

    private:
        PCSCLite* pcsclite_;
        AsyncResult* async_result_;
    };

    // NApi methods
    Napi::Value Start(const Napi::CallbackInfo& info);
    Napi::Value Close(const Napi::CallbackInfo& info);

    // Internal methods
    LONG get_card_readers(AsyncResult* async_result);
    static void HandlerFunction(void* arg);

    // Member variables
    SCARDCONTEXT m_card_context;
    SCARD_READERSTATE m_card_reader_state;
    std::thread m_status_thread;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    bool m_pnp;
    int m_state;
    Napi::ThreadSafeFunction m_tsfn;
    Napi::FunctionReference m_callback;
};

#endif /* PCSCLITE_H */