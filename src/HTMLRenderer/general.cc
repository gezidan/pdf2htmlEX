/*
 * general.cc
 *
 * Handling general stuffs
 *
 * by WangLu
 * 2012.08.14
 */

#include <cstdio>
#include <ostream>
#include <cmath>

#include <splash/SplashBitmap.h>

#include "HTMLRenderer.h"
#include "BackgroundRenderer.h"
#include "namespace.h"
#include "ffw.h"
#include "pdf2htmlEX-config.h"

namespace pdf2htmlEX {

using std::fixed;
using std::flush;
using std::ostream;
using std::max;

static void dummy(void *, enum ErrorCategory, int pos, char *)
{
}

HTMLRenderer::HTMLRenderer(const Param * param)
    :OutputDev()
    ,line_opened(false)
    ,line_buf(this)
    ,preprocessor(param)
    ,image_count(0)
    ,param(param)
{
    if(!(param->debug))
    {
        //disable error function of poppler
        setErrorCallback(&dummy, nullptr);
    }

    ffw_init(param->debug);
    cur_mapping = new int32_t [0x10000];
    cur_mapping2 = new char* [0x100];
    width_list = new int [0x10000];

    /*
     * determine scale factors
     */
    scale_factor1 = max(param->zoom, param->font_size_multiplier);  
    scale_factor2 = (param->zoom) / scale_factor1;
}

HTMLRenderer::~HTMLRenderer()
{ 
    ffw_fin();
    clean_tmp_files();
    delete [] cur_mapping;
    delete [] cur_mapping2;
    delete [] width_list;
}

static GBool annot_cb(Annot *, void *) {
    return false;
};

void HTMLRenderer::process(PDFDoc *doc)
{
    cur_doc = doc;
    xref = doc->getXRef();

    cerr << "Preprocessing: ";
    preprocessor.process(doc);

    cerr << "Working: ";
    BackgroundRenderer * bg_renderer = nullptr;
    if(param->process_nontext)
    {
        // Render non-text objects as image
        // copied from poppler
        SplashColor color;
        color[0] = color[1] = color[2] = 255;

        bg_renderer = new BackgroundRenderer(splashModeRGB8, 4, gFalse, color);
        bg_renderer->startDoc(doc);
    }

    pre_process();

    for(int i = param->first_page; i <= param->last_page ; ++i) 
    {
        if(param->split_pages)
        {
            auto page_fn = str_fmt("%s/%s%d.page", param->dest_dir.c_str(), param->output_filename.c_str(), i);
            html_fout.open((char*)page_fn, ofstream::binary); 
            if(!html_fout)
                throw string("Cannot open ") + (char*)page_fn + " for writing";
            fix_stream(html_fout);
        }

        if(param->process_nontext)
        {
            doc->displayPage(bg_renderer, i, param->h_dpi, param->v_dpi,
                    0, true, false, false,
                    nullptr, nullptr, &annot_cb, nullptr);

            {
                auto fn = str_fmt("%s/p%x.png", (param->single_html ? param->tmp_dir : param->dest_dir).c_str(), i);
                if(param->single_html)
                    add_tmp_file((char*)fn);

                bg_renderer->getBitmap()->writeImgFile(splashFormatPng, 
                        (char*)fn,
                        param->h_dpi, param->v_dpi);
            }
        }

        doc->displayPage(this, i, (param->zoom) * DEFAULT_DPI, (param->zoom) * DEFAULT_DPI,
                0, true, false, false,
                nullptr, nullptr, nullptr, nullptr);

        if(param->split_pages)
        {
            html_fout.close();
        }

        cerr << "." << flush;
    }

    post_process();

    if(bg_renderer)
        delete bg_renderer;

    cerr << endl;
}

void HTMLRenderer::setDefaultCTM(double *ctm)
{
    memcpy(default_ctm, ctm, sizeof(default_ctm));
}

void HTMLRenderer::startPage(int pageNum, GfxState *state) 
{
    this->pageNum = pageNum;
    this->pageWidth = state->getPageWidth();
    this->pageHeight = state->getPageHeight();

    assert((!line_opened) && "Open line in startPage detected!");

    html_fout 
        << "<div class=\"d\" style=\"width:" 
            << (pageWidth) << "px;height:" 
            << (pageHeight) << "px;\">"
        << "<div id=\"p" << pageNum << "\" data-page-no=\"" << pageNum << "\" class=\"p\">"
        << "<div class=\"b\" style=\"";

    if(param->process_nontext)
    {
        html_fout << "background-image:url(";

        {
            if(param->single_html)
            {
                auto path = str_fmt("%s/p%x.png", param->tmp_dir.c_str(), pageNum);
                ifstream fin((char*)path, ifstream::binary);
                if(!fin)
                    throw string("Cannot read background image ") + (char*)path;
                html_fout << "'data:image/png;base64," << base64stream(fin) << "'";
            }
            else
            {
                html_fout << str_fmt("p%x.png", pageNum);
            }
        }

        html_fout << ");background-position:0 0;background-size:" << pageWidth << "px " << pageHeight << "px;background-repeat:no-repeat;";
    }

    html_fout << "\">";
    draw_scale = 1.0;

    cur_font_info = install_font(nullptr);
    cur_font_size = draw_font_size = 0;
    cur_fs_id = install_font_size(cur_font_size);
    
    memcpy(cur_ctm, id_matrix, sizeof(cur_ctm));
    memcpy(draw_ctm, id_matrix, sizeof(draw_ctm));
    cur_tm_id = install_transform_matrix(draw_ctm);

    cur_letter_space = cur_word_space = 0;
    cur_ls_id = install_letter_space(cur_letter_space);
    cur_ws_id = install_word_space(cur_word_space);

    cur_color.r = cur_color.g = cur_color.b = 0;
    cur_color_id = install_color(&cur_color);

    cur_rise = 0;
    cur_rise_id = install_rise(cur_rise);

    cur_tx = cur_ty = 0;
    draw_tx = draw_ty = 0;

    reset_state_change();
    all_changed = true;
}

void HTMLRenderer::endPage() {
    close_line();

    // process links before the page is closed
    cur_doc->processLinks(this, pageNum);

    // close box
    html_fout << "</div>";

    // dump info for js
    // TODO: create a function for this
    // BE CAREFUL WITH ESCAPES
    html_fout << "<div class=\"j\" data-data='{";
    
    //default CTM
    html_fout << "\"ctm\":[";
    for(int i = 0; i < 6; ++i)
    {
        if(i > 0) html_fout << ",";
        html_fout << _round(default_ctm[i]);
    }
    html_fout << "]";

    html_fout << "}'></div>";
    
    // close page
    html_fout << "</div></div>" << endl;
}

void HTMLRenderer::pre_process()
{
    // we may output utf8 characters, so always use binary
    {
        /*
         * If single-html && !split-pages
         * we have to keep the generated css file into a temporary place
         * and embed it into the main html later
         *
         *
         * If single-html && split-page
         * as there's no place to embed the css file, just leave it alone (into param->dest_dir)
         *
         * If !single-html
         * leave it in param->dest_dir
         */

        auto fn = (param->single_html && (!param->split_pages))
            ? str_fmt("%s/__css", param->tmp_dir.c_str())
            : str_fmt("%s/%s", param->dest_dir.c_str(), param->css_filename.c_str());

        if(param->single_html && (!param->split_pages))
            add_tmp_file((char*)fn);

        css_path = (char*)fn,
        css_fout.open(css_path, ofstream::binary);
        if(!css_fout)
            throw string("Cannot open ") + (char*)fn + " for writing";
        fix_stream(css_fout);
    }

    // if split-pages is specified, open & close the file in the process loop
    // if not, open the file here:
    if(!param->split_pages)
    {
        /*
         * If single-html
         * we have to keep the html file (for page) into a temporary place
         * because we'll have to embed css before it
         *
         * Otherwise just generate it 
         */
        auto fn = str_fmt("%s/__pages", param->tmp_dir.c_str());
        add_tmp_file((char*)fn);

        html_path = (char*)fn;
        html_fout.open(html_path, ofstream::binary); 
        if(!html_fout)
            throw string("Cannot open ") + (char*)fn + " for writing";
        fix_stream(html_fout);
    }
}

void HTMLRenderer::post_process()
{
    // close files
    html_fout.close(); 
    css_fout.close();

    //only when split-page, do we have some work left to do
    if(param->split_pages)
        return;

    ofstream output;
    {
        auto fn = str_fmt("%s/%s", param->dest_dir.c_str(), param->output_filename.c_str());
        output.open((char*)fn, ofstream::binary);
        if(!output)
            throw string("Cannot open ") + (char*)fn + " for writing";
        fix_stream(output);
    }

    // apply manifest
    ifstream manifest_fin((char*)str_fmt("%s/%s", param->data_dir.c_str(), MANIFEST_FILENAME.c_str()), ifstream::binary);
    if(!manifest_fin)
        throw "Cannot open the manifest file";

    bool embed_string = false;
    string line;
    while(getline(manifest_fin, line))
    {
        if(line == "\"\"\"")
        {
            embed_string = !embed_string;
            continue;
        }

        if(embed_string)
        {
            output << line << endl;
            continue;
        }

        if(line.empty() || line[0] == '#')
            continue;


        if(line[0] == '@')
        {
            embed_file(output, param->data_dir + "/" + line.substr(1), "", true);
            continue;
        }

        if(line[0] == '$')
        {
            if(line == "$css")
            {
                embed_file(output, css_path, ".css", false);
            }
            else if (line == "$pages")
            {
                ifstream fin(html_path, ifstream::binary);
                if(!fin)
                    throw "Cannot open read the pages";
                output << fin.rdbuf();
            }
            else
            {
                cerr << "Warning: unknown line in manifest: " << line << endl;
            }
            continue;
        }

        cerr << "Warning: unknown line in manifest: " << line << endl;
    }
}

void HTMLRenderer::fix_stream (std::ostream & out)
{
    out << hex;
}

void HTMLRenderer::add_tmp_file(const string & fn)
{
    if(!param->clean_tmp)
        return;

    if(tmp_files.insert(fn).second && param->debug)
        cerr << "Add new temporary file: " << fn << endl;
}

void HTMLRenderer::clean_tmp_files()
{
    if(!param->clean_tmp)
        return;

    for(auto iter = tmp_files.begin(); iter != tmp_files.end(); ++iter)
    {
        const string & fn = *iter;
        remove(fn.c_str());
        if(param->debug)
            cerr << "Remove temporary file: " << fn << endl;
    }

    remove(param->tmp_dir.c_str());
    if(param->debug)
        cerr << "Remove temporary directory: " << param->tmp_dir << endl;
}

void HTMLRenderer::embed_file(ostream & out, const string & path, const string & type, bool copy)
{
    string fn = get_filename(path);
    string suffix = (type == "") ? get_suffix(fn) : type; 
    
    auto iter = EMBED_STRING_MAP.find(make_pair(suffix, (bool)param->single_html));
    if(iter == EMBED_STRING_MAP.end())
    {
        cerr << "Warning: unknown suffix: " << suffix << endl;
        return;
    }
    
    if(param->single_html)
    {
        ifstream fin(path, ifstream::binary);
        if(!fin)
            throw string("Cannot open file ") + path + " for embedding";
        out << iter->second.first << endl
            << fin.rdbuf()
            << iter->second.second << endl;
    }
    else
    {
        out << iter->second.first
            << fn
            << iter->second.second << endl;

        if(copy)
        {
            ifstream fin(path, ifstream::binary);
            if(!fin)
                throw string("Cannot copy file: ") + path;
            auto out_path = param->dest_dir + "/" + fn;
            ofstream out(out_path, ofstream::binary);
            if(!out)
                throw string("Cannot open file ") + path + " for embedding";
            out << fin.rdbuf();
        }
    }
}

const std::string HTMLRenderer::MANIFEST_FILENAME = "manifest";

}// namespace pdf2htmlEX
