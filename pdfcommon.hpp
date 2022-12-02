/*
 * Copyright 2022 Jussi Pakkanen
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

enum BuiltinFonts {
    FONT_TIMES_ROMAN,
    FONT_HELVETICA,
    FONT_COURIER,
    FONT_TIMES_ROMAN_BOLD,
    FONT_HELVETICA_BOLD,
    FONT_COURIER_BOLD,
    FONT_TIMES_ROMAN_ITALIC,
    FONT_HELVETICA_OBLIQUE,
    FONT_COURIER_OBLIQUE,
};

class LimitDouble {
public:
    LimitDouble() : value(minval) {}

    // Not explicit, because we want the following to work for convenience:
    // DeviceRGBColor{0.0, 0.3, 1.0}
    LimitDouble(double new_val) : value(new_val) { clamp(); }

    double v() const { return value; }

private:
    constexpr static double maxval = 1.0;
    constexpr static double minval = 0.0;

    void clamp() {
        if(value < minval) {
            value = minval;
        }
        if(value > maxval) {
            value = maxval;
        }
    }

    double value;
};

struct DeviceRGBColor {
    LimitDouble r;
    LimitDouble g;
    LimitDouble b;
};

struct DeviceGrayColor {
    LimitDouble v;
};

struct DeviceCMYKColor {
    LimitDouble c;
    LimitDouble m;
    LimitDouble y;
    LimitDouble k;
};