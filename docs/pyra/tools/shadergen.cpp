// Offline renderer for default.shader.fs. Mirrors GfxRenderingAPIOGL::BuildFsShader's context so
// the template can be checked without a GPU, a ROM, or the device.
#include <prism/processor.h>
#include <fstream>
#include <sstream>
#include <iostream>

enum {
    SHADER_0, SHADER_INPUT_1, SHADER_INPUT_2, SHADER_INPUT_3, SHADER_INPUT_4,
    SHADER_INPUT_5, SHADER_INPUT_6, SHADER_INPUT_7, SHADER_TEXEL0, SHADER_TEXEL0A,
    SHADER_TEXEL1, SHADER_TEXEL1A, SHADER_1, SHADER_COMBINED, SHADER_NOISE
};

static prism::ContextTypes* stub_append_formula(prism::ContextTypes*, prism::ContextTypes*,
                                                prism::ContextTypes* a_single, prism::ContextTypes* a_mult,
                                                prism::ContextTypes* a_mix, prism::ContextTypes*,
                                                prism::ContextTypes*, prism::ContextTypes*,
                                                prism::ContextTypes*) {
    auto b = [](prism::ContextTypes* v) {
        return std::holds_alternative<int>(*v) && std::get<int>(*v) == 1;
    };
    const char* form = b(a_single) ? "SINGLE" : b(a_mult) ? "MULTIPLY" : b(a_mix) ? "MIX" : "FULL";
    return new prism::ContextTypes{ std::string("/*") + form + "*/ vec4(0.0)" };
}

struct Variant {
    const char* name;
    int c[2][2][4];
    bool opt_alpha, opt_2cyc, wrap_mid, wrap_out, float_tex_size;
    bool do_single[2][2], do_multiply[2][2], do_mix[2][2], color_alpha_same[2];
};

int main(int argc, char** argv) {
    std::ifstream in(argv[1]);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string source = ss.str();

    static Variant vs[] = {
        // TEXEL0 * SHADE, one cycle, alpha: the reduced multiply form, provably in range.
        { "1cyc multiply, wrap elided",
          { { { SHADER_TEXEL0, SHADER_0, SHADER_INPUT_1, SHADER_0 },
              { SHADER_TEXEL0, SHADER_0, SHADER_INPUT_1, SHADER_0 } },
            { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } },
          true, false, /*wrap_mid*/ false, /*wrap_out*/ false, true,
          { { 0, 0 }, { 0, 0 } }, { { 1, 1 }, { 0, 0 } }, { { 0, 0 }, { 0, 0 } }, { true, false } },
        // The same shape but with the wrap kept: what arm A (CVAR_SHADER_OPTS=0) must still emit.
        { "1cyc multiply, wrap kept (arm A)",
          { { { SHADER_TEXEL0, SHADER_0, SHADER_INPUT_1, SHADER_0 },
              { SHADER_TEXEL0, SHADER_0, SHADER_INPUT_1, SHADER_0 } },
            { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } },
          true, false, true, true, false,
          { { 0, 0 }, { 0, 0 } }, { { 1, 1 }, { 0, 0 } }, { { 0, 0 }, { 0, 0 } }, { true, false } },
        // Full (A-B)*C+D in both cycles: both wraps load-bearing.
        { "2cyc full form, both wraps kept",
          { { { SHADER_TEXEL0, SHADER_TEXEL1, SHADER_INPUT_1, SHADER_INPUT_2 },
              { SHADER_TEXEL0, SHADER_TEXEL1, SHADER_INPUT_1, SHADER_INPUT_2 } },
            { { SHADER_COMBINED, SHADER_0, SHADER_INPUT_1, SHADER_INPUT_2 },
              { SHADER_COMBINED, SHADER_0, SHADER_INPUT_1, SHADER_INPUT_2 } } },
          true, true, true, true, true,
          { { 0, 0 }, { 0, 0 } }, { { 0, 0 }, { 0, 0 } }, { { 0, 0 }, { 0, 0 } }, { true, true } },
        // Two cycles, both reduced: nothing left to wrap anywhere.
        { "2cyc reduced, both wraps elided",
          { { { SHADER_TEXEL0, SHADER_0, SHADER_INPUT_1, SHADER_0 },
              { SHADER_TEXEL0, SHADER_0, SHADER_INPUT_1, SHADER_0 } },
            { { SHADER_COMBINED, SHADER_0, SHADER_INPUT_2, SHADER_0 },
              { SHADER_COMBINED, SHADER_0, SHADER_INPUT_2, SHADER_0 } } },
          true, true, false, false, true,
          { { 0, 0 }, { 0, 0 } }, { { 1, 1 }, { 1, 1 } }, { { 0, 0 }, { 0, 0 } }, { true, true } },
    };

    for (auto& v : vs) {
        bool textures[2] = { true, false };
        bool masks[2] = { false, false };
        bool blend[2] = { false, false };
        bool clamp[2][2] = { { true, true }, { false, false } };

        prism::Processor processor;
        prism::ContextItems ctx = {
            { "o_c", M_ARRAY(v.c, int, 2, 2, 4) },
            { "o_alpha", v.opt_alpha },
            { "o_fog", false },
            { "o_texture_edge", false },
            { "o_noise", false },
            { "o_2cyc", v.opt_2cyc },
            { "o_alpha_threshold", false },
            { "o_invisible", false },
            { "o_grayscale", false },
            { "o_textures", M_ARRAY(textures, bool, 2) },
            { "o_masks", M_ARRAY(masks, bool, 2) },
            { "o_blend", M_ARRAY(blend, bool, 2) },
            { "o_clamp", M_ARRAY(clamp, bool, 2, 2) },
            { "o_inputs", 2 },
            { "o_do_mix", M_ARRAY(v.do_mix, bool, 2, 2) },
            { "o_do_single", M_ARRAY(v.do_single, bool, 2, 2) },
            { "o_do_multiply", M_ARRAY(v.do_multiply, bool, 2, 2) },
            { "o_color_alpha_same", M_ARRAY(v.color_alpha_same, bool, 2) },
            { "FILTER_THREE_POINT", 2 },
            { "FILTER_LINEAR", 1 },
            { "FILTER_NONE", 0 },
            { "srgb_mode", false },
            { "SHADER_0", SHADER_0 },
            { "SHADER_INPUT_1", SHADER_INPUT_1 },
            { "SHADER_INPUT_2", SHADER_INPUT_2 },
            { "SHADER_INPUT_3", SHADER_INPUT_3 },
            { "SHADER_INPUT_4", SHADER_INPUT_4 },
            { "SHADER_INPUT_5", SHADER_INPUT_5 },
            { "SHADER_INPUT_6", SHADER_INPUT_6 },
            { "SHADER_INPUT_7", SHADER_INPUT_7 },
            { "SHADER_TEXEL0", SHADER_TEXEL0 },
            { "SHADER_TEXEL0A", SHADER_TEXEL0A },
            { "SHADER_TEXEL1", SHADER_TEXEL1 },
            { "SHADER_TEXEL1A", SHADER_TEXEL1A },
            { "SHADER_1", SHADER_1 },
            { "SHADER_COMBINED", SHADER_COMBINED },
            { "SHADER_NOISE", SHADER_NOISE },
            { "o_three_point_filtering", false },
            { "o_wrap_mid", v.wrap_mid },
            { "o_wrap_out", v.wrap_out },
            { "o_float_tex_size", v.float_tex_size },
            { "append_formula", (InvokeFunc)stub_append_formula },
            { "GLSL_VERSION", "#version 100\nprecision mediump float;" },
            { "attr", "varying" },
            { "opengles", true },
            { "opengles2", true },
            { "frag_out", false },
            { "core_opengl", false },
            { "texture", "texture2D" },
            { "vOutColor", "gl_FragColor" },
        };
        processor.populate(ctx);
        processor.load(source);
        std::cout << "//================ " << v.name << "\n" << processor.process() << "\n";
    }
    return 0;
}
