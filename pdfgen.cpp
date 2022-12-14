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
#include <imageops.hpp>
#include <utils.hpp>
#include <cstring>
#include <cerrno>
#include <cassert>
#include <lcms2.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_FONT_FORMATS_H
#include FT_OPENTYPE_VALIDATE_H
#include <stdexcept>
#include <array>
#include <memory>
#include <fmt/core.h>

namespace {

const char PDF_header[] = "%PDF-1.7\n\xe5\xf6\xc4\xd6\n";

const std::array<const char *, 9> font_names{
    "Times-Roman",
    "Helvetica",
    "Courier",
    "Times-Roman-Bold",
    "Helvetica-Bold",
    "Courier-Bold",
    "Times-Italic",
    "Helvetica-Oblique",
    "Courier-Oblique",
};

const char default_srgb_profile[] = "/usr/share/color/icc/ghostscript/srgb.icc";
const char default_gray_profile[] = "/usr/share/color/icc/ghostscript/sgray.icc";
const char default_cmyk_profile[] =
    "/home/jpakkane/Downloads/temp/Adobe ICC Profiles (end-user)/CMYK/UncoatedFOGRA29.icc";

void write_box(auto &appender, const char *boxname, const PdfBox &box) {
    fmt::format_to(appender, "  /{} [ {} {} {} {} ]\n", boxname, box.x, box.y, box.w, box.h);
}

std::string fontname2pdfname(std::string_view original) {
    std::string out;
    out.reserve(original.size());
    for(const auto c : original) {
        if(c == ' ') {
            continue;
        }
        if(c == '\\') {
            continue;
        }
        out += c;
    }
    // FIXME: might need to escape other special characters as well.
    return out;
}

std::string
build_width_array(FT_Face face, const int start_char, const int one_past_the_end_end_char) {
    std::string arr{"[ "};
    assert(one_past_the_end_end_char > start_char);
    arr.reserve((one_past_the_end_end_char - start_char) * 10);
    auto bi = std::back_inserter(arr);
    const auto load_flags =
        FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH;
    //    static_assert(load_flags == 522);
    for(int i = start_char; i < one_past_the_end_end_char; ++i) {
        auto glyph_index = FT_Get_Char_Index(face, i);
        auto error = FT_Load_Glyph(face, glyph_index, load_flags);
        if(error != 0) {
            throw std::runtime_error(FT_Error_String(error));
        }
        fmt::format_to(bi, "{} ", face->glyph->metrics.horiAdvance);
    }
    arr += "]";
    return arr;
}

} // namespace

FT_Error guarded_face_close(FT_Face face) {
    // Freetype segfaults if you give it a null pointer.
    if(face) {
        return FT_Done_Face(face);
    }
    return 0;
}

LcmsHolder::~LcmsHolder() { deallocate(); }

void LcmsHolder::deallocate() {
    if(h) {
        cmsCloseProfile(h);
    }
    h = nullptr;
}

PdfGen::PdfGen(const char *ofname, const PdfGenerationData &d)
    : opts{d}, cm{default_srgb_profile, default_gray_profile, default_cmyk_profile} {
    ofile = fopen(ofname, "wb");
    if(!ofile) {
        throw std::runtime_error(strerror(errno));
    }
    auto error = FT_Init_FreeType(&ft);
    if(error) {
        throw std::runtime_error(FT_Error_String(error));
    }
    write_header();
    write_info();
    create_separation("All", DeviceCMYKColor{1.0, 1.0, 1.0, 1.0});
    rgb_profile_obj = store_icc_profile(cm.get_rgb(), 3);
    gray_profile_obj = store_icc_profile(cm.get_gray(), 1);
    cmyk_profile_obj = store_icc_profile(cm.get_cmyk(), 4);
}

PdfGen::~PdfGen() {
    font_objects.clear();
    auto error = FT_Done_FreeType(ft);
    if(error) {
        fprintf(stderr, "Closing FreeType failed: %s\n", FT_Error_String(error));
    }
    ft = nullptr;
    try {
        close_file();
    } catch(const std::exception &e) {
        fprintf(stderr, "%s", e.what());
        fclose(ofile);
        return;
    } catch(...) {
        fprintf(stderr, "Unexpected error.\n");
        fclose(ofile);
    }

    if(fflush(ofile) != 0) {
        perror("Flushing file contents failed");
    }
    if(fclose(ofile) != 0) {
        perror("Closing output file failed");
    }
}

int32_t PdfGen::store_icc_profile(std::string_view contents, int32_t num_channels) {
    std::string compressed = flate_compress(contents);
    std::string buf;
    fmt::format_to(std::back_inserter(buf),
                   R"(<<
  /Filter /FlateDecode
  /Length {}
  /N {}
>>
stream
)",
                   compressed.size(),
                   num_channels);
    buf += compressed;
    buf += "\nendstream\n";
    return add_object(buf);
}

void PdfGen::write_bytes(const char *buf, size_t buf_size) {
    if(fwrite(buf, 1, buf_size, ofile) != buf_size) {
        throw std::runtime_error(strerror(errno));
    }
}

void PdfGen::write_header() { write_bytes(PDF_header, strlen(PDF_header)); }

void PdfGen::write_info() {
    std::string obj_data{"<<\n"};
    if(!opts.title.empty()) {
        obj_data += "  /Title ";
        obj_data += utf8_to_pdfmetastr(opts.title);
        obj_data += "\n";
    }
    if(!opts.author.empty()) {
        obj_data += "  /Author ";
        obj_data += utf8_to_pdfmetastr(opts.author);
        obj_data += "\n";
    }
    obj_data += "  /Producer (PDF Testbed generator)\n";
    obj_data += "  /CreationDate ";
    obj_data += current_date_string();
    obj_data += '\n';
    obj_data += ">>\n";
    add_object(obj_data);
}

void PdfGen::close_file() {
    write_pages();
    write_catalog();
    const int64_t xref_offset = ftell(ofile);
    write_cross_reference_table();
    write_trailer(xref_offset);
}

void PdfGen::write_pages() {
    const auto pages_obj_num = (int32_t)(object_offsets.size() + pages.size() + 1);

    std::string buf;

    std::vector<int32_t> page_objects;
    auto buf_append = std::back_inserter(buf);
    for(const auto &i : pages) {
        buf.clear();
        fmt::format_to(buf_append,
                       R"(<<
  /Type /Page
  /Parent {} 0 R
)",
                       pages_obj_num);
        write_box(buf_append, "MediaBox", opts.mediabox);

        if(opts.cropbox) {
            write_box(buf_append, "CropBox", *opts.cropbox);
        }
        if(opts.bleedbox) {
            write_box(buf_append, "BleedBox", *opts.bleedbox);
        }
        if(opts.trimbox) {
            write_box(buf_append, "TrimBox", *opts.trimbox);
        }
        if(opts.artbox) {
            write_box(buf_append, "ArtBox", *opts.artbox);
        }
        fmt::format_to(buf_append,
                       R"(  /Contents {} 0 R
  /Resources {} 0 R
>>
)",
                       i.commands_obj_num,
                       i.resource_obj_num);

        page_objects.push_back(add_object(buf));
    }
    buf = R"(<<
  /Type /Pages
  /Kids [
)";
    for(const auto &i : page_objects) {
        fmt::format_to(buf_append, "    {} 0 R\n", i);
    }
    fmt::format_to(buf_append, "  ]\n  /Count {}\n>>\n", page_objects.size());
    const auto actual_number = add_object(buf);
    if(actual_number != pages_obj_num) {
        throw std::runtime_error("Buggy McBugFace!");
    }
}

void PdfGen::write_catalog() {
    const int32_t pages_obj_num = (int32_t)object_offsets.size();
    std::string buf;
    fmt::format_to(std::back_inserter(buf),
                   R"(<<
  /Type /Catalog
  /Pages {} 0 R
>>
)",
                   pages_obj_num);
    add_object(buf);
}

void PdfGen::write_cross_reference_table() {
    std::string buf;
    fmt::format_to(
        std::back_inserter(buf),
        R"(xref
0 {}
0000000000 65535 f{}
)",
        object_offsets.size() + 1,
        " "); // Qt Creator removes whitespace at the end of lines but it is significant here.
    for(const auto &i : object_offsets) {
        fmt::format_to(std::back_inserter(buf), "{:010} 00000 n \n", i);
    }
    write_bytes(buf);
}

void PdfGen::write_trailer(int64_t xref_offset) {
    const int32_t info = 1;                     // Info object is the first printed.
    const int32_t root = object_offsets.size(); // Root object is the last one printed.
    std::string buf;
    fmt::format_to(std::back_inserter(buf),
                   R"(trailer
<<
  /Size {}
  /Root {} 0 R
  /Info {} 0 R
>>
startxref
{}
%%EOF
)",
                   object_offsets.size() + 1,
                   root,
                   info,
                   xref_offset);
    write_bytes(buf);
}

PdfPage PdfGen::new_page() { return PdfPage(this, &cm); }

void PdfGen::add_page(std::string_view resource_data, std::string_view page_data) {
    const auto resource_num = add_object(resource_data);
    const auto page_num = add_object(page_data);
    pages.emplace_back(PageOffsets{resource_num, page_num});
}

int32_t PdfGen::add_object(std::string_view object_data) {
    auto object_num = (int32_t)object_offsets.size() + 1;
    object_offsets.push_back(ftell(ofile));
    const int bufsize = 128;
    char buf[bufsize];
    snprintf(buf, bufsize, "%d 0 obj\n", object_num);
    write_bytes(buf);
    write_bytes(object_data);
    write_bytes("endobj\n");
    return object_num;
}

ImageId PdfGen::load_image(const char *fname) {
    auto image = load_image_file(fname);
    std::string buf;
    int32_t smask_id = -1;
    if(image.alpha) {
        auto compressed = flate_compress(*image.alpha);
        fmt::format_to(std::back_inserter(buf),
                       R"(<<
  /Type /XObject
  /Subtype /Image
  /Width {}
  /Height {}
  /ColorSpace /DeviceGray
  /BitsPerComponent 8
  /Length {}
  /Filter /FlateDecode
>>
stream
)",
                       image.w,
                       image.h,
                       compressed.size());
        buf += compressed;
        buf += "\nendstream\n";
        smask_id = add_object(buf);
        buf.clear();
    }
    switch(opts.output_colorspace) {
    case PDF_DEVICE_RGB: {
        const auto compressed = flate_compress(image.pixels);
        fmt::format_to(std::back_inserter(buf),
                       R"(<<
  /Type /XObject
  /Subtype /Image
  /ColorSpace [/ICCBased {} 0 R]
  /Width {}
  /Height {}
  /BitsPerComponent 8
  /Length {}
  /Filter /FlateDecode
)",
                       rgb_profile_obj,
                       image.w,
                       image.h,
                       compressed.size());
        if(smask_id >= 0) {
            fmt::format_to(std::back_inserter(buf), "  /SMask {} 0 R\n", smask_id);
        }
        buf += ">>\nstream\n";
        buf += compressed;
        buf += "\nendstream\n";
        auto im_id = add_object(buf);
        image_info.emplace_back(ImageInfo{{image.w, image.h}, im_id});
        break;
    }
    case PDF_DEVICE_GRAY: {
        std::string converted_pixels = cm.rgb_pixels_to_gray(image.pixels);
        const auto compressed = flate_compress(converted_pixels);
        fmt::format_to(std::back_inserter(buf),
                       R"(<<
  /Type /XObject
  /Subtype /Image
  /ColorSpace [/ICCBased {} 0 R]
  /Width {}
  /Height {}
  /BitsPerComponent 8
  /Length {}
  /Filter /FlateDecode
)",
                       gray_profile_obj, // FIXME, maybe this should be DeviceGray?
                       image.w,
                       image.h,
                       compressed.size());
        if(smask_id >= 0) {
            fmt::format_to(std::back_inserter(buf), "  /SMask {} 0 R\n", smask_id);
        }
        buf += ">>\nstream\n";
        buf += compressed;
        buf += "\nendstream\n";
        auto im_id = add_object(buf);
        image_info.emplace_back(ImageInfo{{image.w, image.h}, im_id});
        break;
    }
    case PDF_DEVICE_CMYK: {
        std::string converted_pixels = cm.rgb_pixels_to_cmyk(image.pixels);
        const auto compressed = flate_compress(converted_pixels);
        fmt::format_to(std::back_inserter(buf),
                       R"(<<
  /Type /XObject
  /Subtype /Image
  /ColorSpace [/ICCBased {} 0 R]
  /Width {}
  /Height {}
  /BitsPerComponent 8
  /Length {}
  /Filter /FlateDecode
)",
                       cmyk_profile_obj, // FIXME, maybe this should be DeviceGray?
                       image.w,
                       image.h,
                       compressed.size());
        if(smask_id >= 0) {
            fmt::format_to(std::back_inserter(buf), "  /SMask {} 0 R\n", smask_id);
        }
        buf += ">>\nstream\n";
        buf += compressed;
        buf += "\nendstream\n";
        auto im_id = add_object(buf);
        image_info.emplace_back(ImageInfo{{image.w, image.h}, im_id});
        break;
    }
    default:
        throw std::runtime_error("Not implemented.");
    }
    return ImageId{(int32_t)image_info.size() - 1};
}

FontId PdfGen::load_font(const char *fname) {
    FT_Face face;
    auto error = FT_New_Face(ft, fname, 0, &face);
    if(error) {
        // By default Freetype is compiled without error strings. Yay!
        throw std::runtime_error(fmt::format("Freetype error {}.", error));
    }
    std::unique_ptr<FT_FaceRec_, FT_Error (*)(FT_Face)> fcloser(face, guarded_face_close);
    const char *font_format = FT_Get_Font_Format(face);
    if(!font_format) {
        throw std::runtime_error(fmt::format("Could not determine format of font file {}.", fname));
    }
    if(strcmp(font_format, "TrueType")) { // != 0 && strcmp(font_format, "CFF") != 0) {
        throw std::runtime_error(
            fmt::format("Only TrueType fonts are supported. {} is a {} font.", fname, font_format));
    }
    FT_Bytes base = nullptr;
    error = FT_OpenType_Validate(face, FT_VALIDATE_BASE, &base, nullptr, nullptr, nullptr, nullptr);
    if(base) {
        FT_OpenType_Free(face, base);
    }
    if(!error) {
        throw std::runtime_error(fmt::format("Font file {} is an OpenType font. Only TrueType "
                                             "fonts are supported. Freetype error {}.",
                                             fname,
                                             error));
    }
    auto bytes = load_file(fname);
    auto compressed_bytes = flate_compress(bytes);
    std::string objbuf = fmt::format(R"(<<
  /Length {}
  /Length1 {}
  /Filter /FlateDecode
>>
stream
)",
                                     compressed_bytes.size(),
                                     bytes.size());
    objbuf += compressed_bytes;
    objbuf += "\nendstream\n";
    auto font_file_obj = add_object(objbuf);
    const uint32_t fflags = 32;
    objbuf = fmt::format(R"(<<
  /Type /FontDescriptor
  /FontName /{}
  /FontFamily ({})
  /Flags {}
  /FontBBox [ {} {} {} {} ]
  /ItalicAngle {}
  /Ascent {}
  /Descent {}
  /CapHeight {}
  /StemH {}
  /StemV {}
  /FontFile2 {} 0 R
>>
)",
                         fontname2pdfname(FT_Get_Postscript_Name(face)),
                         face->family_name,
                         fflags,
                         face->bbox.xMin,
                         face->bbox.yMin,
                         face->bbox.xMax,
                         face->bbox.yMax,
                         0, // Cairo always sets this to zero.
                         face->ascender,
                         face->descender,
                         face->bbox.yMax, // Copying what Cairo does.
                         80,              // Cairo always sets these to 80.
                         80,
                         font_file_obj);
    auto font_descriptor_obj = add_object(objbuf);

    const int start_char = 0;
    const int end_char = 0xFFFD; // Unicode replacement character.
    auto width_arr = build_width_array(face, start_char, end_char + 1);
    objbuf = fmt::format(R"(<<
  /Type /Font
  /Subtype /TrueType
  /BaseFont /{}
  /FirstChar {}
  /LastChar {}
  /Widths {}
  /FontDescriptor {} 0 R
>>
)",
                         FT_Get_Postscript_Name(face),
                         start_char,
                         end_char,
                         width_arr,
                         font_descriptor_obj);
    auto fontobj = add_object(objbuf);
    font_objects.push_back(
        FontInfo{font_file_obj, font_descriptor_obj, fontobj, std::move(fcloser)});
    FontId fid{(int32_t)font_objects.size() - 1};
    return fid;
}

FontId PdfGen::get_builtin_font_id(BuiltinFonts font) {
    auto it = builtin_fonts.find(font);
    if(it != builtin_fonts.end()) {
        return it->second;
    }
    std::string font_dict;
    fmt::format_to(std::back_inserter(font_dict),
                   R"(<<
  /Type /Font
  /Subtype /Type1
  /BaseFont /{}
>>
)",
                   font_names[font]);
    font_objects.push_back(FontInfo{-1, -1, add_object(font_dict), {nullptr, guarded_face_close}});
    auto fontid = FontId{(int32_t)font_objects.size() - 1};
    builtin_fonts[font] = fontid;
    return fontid;
}

SeparationId PdfGen::create_separation(std::string_view name, const DeviceCMYKColor &fallback) {
    std::string stream = fmt::format(R"({{ dup {} mul
exch {} exch dup {} mul
exch {} mul
}}
)",
                                     fallback.c.v(),
                                     fallback.m.v(),
                                     fallback.y.v(),
                                     fallback.k.v());
    std::string buf = fmt::format(R"(<<
  /FunctionType 4
  /Domain [ 0.0 1.0 ]
  /Range [ 0.0 1.0 0.0 1.0 0.0 1.0 0.0 1.0 ]
  /Length {}
>>
stream
{}
endstream
)",
                                  stream.length(),
                                  stream);
    auto fn_num = add_object(buf);
    buf.clear();
    fmt::format_to(std::back_inserter(buf),
                   R"([
  /Separation
    /{}
    /DeviceCMYK
    {} 0 R
]
)",
                   name,
                   fn_num);
    separation_objects.push_back(add_object(buf));
    return SeparationId{(int32_t)separation_objects.size() - 1};
}

uint32_t PdfGen::glyph_for_codepoint(FT_Face face, uint32_t ucs4) {
    assert(face);
    return FT_Get_Char_Index(face, ucs4);
}
