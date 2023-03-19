#include "SSD_mock.hh"

SSD_device_mock::SSD_device_mock(size_t lba_num, size_t channel_size)
{
    flash.resize(lba_num, {0});
}

