#!/usr/bin/env python
# -*- coding: utf-8 -*-
# Copyright (c) 2025 Huawei Device Co., Ltd.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import time
from devicetest.core.test_case import TestCase, Step
from hypium import *
from hypium.action.os_hypium.device_logger import DeviceLogger
from aw import Common


SubStartupAppspawnDevicedebug0900(TestCase):

    def __init__(self, controllers):
        self.tag = self.__class__.__name__
        TestCase.__init__(self, self.tag, controllers)
        self.driver = UiDriver(self.device1)

    def setup(self):
        Step(self.devices[0].device_id)
        global device
        device = self.driver.shell("param get const.product.model")
        device = device.replace("\n", "").replace(" ", "")
        device = str(device)
        Step(device)
        # 解锁屏幕
        wake = self.driver.Screen.is_on()
        time.sleep(0.5)
        if wake:
            self.driver.ScreenLock.unlock()
        else:
            self.driver.Screen.wake_up()
            self.driver.ScreenLock.unlock()
        self.driver.Screen.enable_stay_awake()

    def process(self):
        devicedebug_help_dict = [
            "usage: devicedebug <command> <options>",
            "These are common devicedebug commands list:",
            "         help              list available commands",
            "         kill              send a signal(1-64) to a process"
        ]
        result_dict = list(filter(lambda x: x != "\r", self.driver.shell("devicedebug help").split("\n")))
        result_dict = list(filter(None, result_dict))
        for result in result_dict:
            self.driver.Assert.contains(devicedebug_help_dict, result.rstrip("\r"))

    def teardown(self):
        Step("收尾工作.................")