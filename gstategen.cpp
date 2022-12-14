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

#include <pdfgen.hpp>
#include <fmt/core.h>

int main(int argc, char **argv) {
    if(argc != 3) {
        printf("%s <bg file> <fg file>\n", argv[0]);
        return 1;
    }
    PdfGenerationData opts;
    opts.output_colorspace = PDF_DEVICE_RGB;
    opts.page_size.h = 300;
    opts.page_size.w = 300;
    opts.mediabox.x = opts.mediabox.y = 0;
    opts.mediabox.w = opts.page_size.w;
    opts.mediabox.h = opts.page_size.h;
    PdfGen gen("gstate.pdf", opts);
    auto ctx = gen.new_page();
    GraphicsState gs;
    gs.blend_mode = BM_MULTIPLY;
    gs.intent = RI_PERCEPTUAL;
    auto bg_img = gen.load_image(argv[1]);
    auto fg_img = gen.load_image(argv[2]);
    ctx.cmd_q();
    ctx.scale(opts.page_size.w, opts.page_size.h);
    ctx.draw_image(bg_img);
    ctx.cmd_Q();
    // There are 16 blend modes.
    const int imsize = 40;
    BlendMode bm = BM_NORMAL;
    for(int j = 3; j >= 0; --j) {
        for(int i = 0; i < 4; ++i) {
            GraphicsState gs;
            gs.blend_mode = bm;
            auto gs_name = fmt::format("bm{}", (int)bm);
            ctx.add_graphics_state(gs_name, gs);
            ctx.cmd_q();
            ctx.cmd_gs(gs_name);
            ctx.translate((i + 0.5) * 1.5 * imsize, (j + 0.5) * 1.5 * imsize);
            ctx.scale(imsize, imsize);
            ctx.draw_image(fg_img);
            ctx.cmd_Q();
            bm = (BlendMode)((int)bm + 1);
        }
    }
    return 0;
}
