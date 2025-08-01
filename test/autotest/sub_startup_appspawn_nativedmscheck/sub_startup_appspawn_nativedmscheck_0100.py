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

import time
from devicetest.core.test_case import TestCase, Step, CheckPoint
from devicetest.core.test_case import Step, TestCase
from hypium.model import UiParam, WindowFilter
import os


class SubStartupAppspawnNativedmscheck0100(TestCase):

    def __init__(self, controllers):
        self.tests = [
            "test_step1"
        ]
        TestCase.__init__(self, self.TAG, controllers)
        self.driver = UiDriver(self.device1)

    def setup(self):
        Step("预置工作:初始化手机开始.................")
        self.driver = UiDriver(self.device1)
        Step("预置工作:唤醒屏幕.................")
        self.driver.enable_auto_wakeup(self.device1)
        Step("预置工作:滑动解锁.................")
        self.driver.swipe(UiParam.UP, side=UiParam.BOTTOM)
        Step('设置屏幕常亮')
        self.driver.Screen.enable_stay_awake()

    def test_step1(self):
        Step("步骤1：创建公有hnp存放目录")
        self.driver.shell("mkdir -p data/no_sign/public")
        Step("步骤2：导入安装包文件hnp")
        path = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
        hnp = os.path.abspath(os.path.join(
            os.path.join(path, "testFile"), 'SUB_STARTUP_APPSPAWN_NATIVE/no_sign/hnpsample.hnp'))
        hap = os.path.abspath(
            os.path.join(os.path.join(path, "testFile"),
                         'SUB_STARTUP_APPSPAWN_NATIVE/no_sign/entry-default-signedArm64.hap'))
        self.driver.push_file(hnp, "data/no_sign/public/")
        self.driver.push_file(hap, "data/no_sign")
        Step("步骤3：执行安装命令")
        result = self.driver.shell(
            "hnp install -u 100 -p wechat_sign -i /data/no_sign -s /data/no_sign/entry-default-signedArm64-test11.hap -a arm64 -f")
        Step("步骤4：预期结果校验")
        self.driver.Assert.contains(result, "hnp uninstall start now! name=hnpsample")
        self.driver.Assert.contains(result, "native manager process exit. ret=8393475")

    def teardown(self):
        self.driver.shell("rm -rf data/no_sign")
