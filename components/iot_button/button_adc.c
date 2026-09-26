// Copyright 2020 Espressif Systems (Shanghai) Co. Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "button_adc.h"
#include "esp_err.h"

esp_err_t button_adc_init(const button_adc_config_t *config)
{
    return ESP_OK;
}

esp_err_t button_adc_deinit(adc1_channel_t channel, int button_index)
{
    return ESP_OK;
}

uint8_t button_adc_get_key_level(void *button_index)
{
    return 0;
}
