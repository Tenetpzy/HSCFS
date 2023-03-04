#include "communication/comm_api.h"
#include "communication/channel.h"
#include "communication/dev.h"
#include "communication/session.h"

int comm_submit_async_read_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count,
    comm_async_cb_func cb_func, void *cb_arg)
{
    int ret = 0;
    comm_channel_handle channel = comm_channel_controller_get_channel(dev->channel_ctrlr);
    comm_session_cmd_ctx *session_ctx = new_comm_session_async_cmd_ctx(channel, SESSION_IO_CMD,
        cb_func, cb_arg, &ret);

    comm_channel_send_read_cmd(channel, buffer, lba, lba_count, 
        comm_session_polling_thread_callback, cb_arg);

    comm_session_env_submit_cmd_ctx(session_ctx);

    return ret;
}