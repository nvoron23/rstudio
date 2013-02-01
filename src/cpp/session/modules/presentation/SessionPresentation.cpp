/*
 * SessionPresentation.cpp
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */


#include "SessionPresentation.hpp"

#include <iostream>

#include <boost/utility.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <core/Exec.hpp>
#include <core/FileSerializer.hpp>
#include <core/HtmlUtils.hpp>
#include <core/markdown/Markdown.hpp>
#include <core/text/TemplateFilter.hpp>
#include <core/system/Process.hpp>

#include <r/RExec.hpp>
#include <r/RSexp.hpp>
#include <r/RRoutines.hpp>

#include <session/SessionModuleContext.hpp>
#include <session/projects/SessionProjects.hpp>

#include "PresentationState.hpp"

#include "SlideParser.hpp"
#include "SlideRenderer.hpp"

using namespace core;

namespace session {
namespace modules { 
namespace presentation {

namespace {

FilePath presentationResourcesPath()
{
   return session::options().rResourcesPath().complete("presentation");
}

SEXP rs_showPresentation(SEXP directorySEXP,
                         SEXP tabCaptionSEXP,
                         SEXP authorModeSEXP)
{
   try
   {
      if (session::options().programMode() == kSessionProgramModeServer)
      {
         // validate path
         FilePath dir(r::sexp::asString(directorySEXP));
         if (!dir.exists())
            throw r::exec::RErrorException("Directory " + dir.absolutePath() +
                                           " does not exist.");

         // initialize state
         presentation::state::init(dir,
                                   r::sexp::asString(tabCaptionSEXP),
                                   r::sexp::asLogical(authorModeSEXP));

         // notify the client
         ClientEvent event(client_events::kShowPresentationPane,
                           presentation::state::asJson());
         module_context::enqueClientEvent(event);
      }
      else
      {
         throw r::exec::RErrorException("Presentations are not supported "
                                        "in desktop mode.");
      }
   }
   catch(const r::exec::RErrorException& e)
   {
      r::exec::error(e.message());
   }

   return R_NilValue;
}

core::Error setPresentationSlideIndex(const json::JsonRpcRequest& request,
                                      json::JsonRpcResponse*)
{
   int index;
   Error error = json::readParam(request.params, 0, &index);
   if (error)
      return error;

   presentation::state::setSlideIndex(index);

   return Success();
}

core::Error closePresentationPane(const json::JsonRpcRequest&,
                                 json::JsonRpcResponse*)
{
   presentation::state::clear();

   return Success();
}

class ResourceFiles : boost::noncopyable
{
private:
   ResourceFiles() {}

public:
   std::string get(const std::string& path)
   {
      return module_context::resourceFileAsString(path);
   }

private:
   friend ResourceFiles& resourceFiles();
};

ResourceFiles& resourceFiles()
{
   static ResourceFiles instance;
   return instance;
}


std::string mathjaxIfRequired(const std::string& contents)
{
   if (markdown::isMathJaxRequired(contents))
      return resourceFiles().get("presentation/mathjax.html");
   else
      return std::string();
}

void handleRangeRequest(const FilePath& targetFile,
                        const http::Request& request,
                        http::Response* pResponse)
{
   // read the file in from disk
   std::string contents;
   Error error = core::readStringFromFile(targetFile, &contents);
   if (error)
      pResponse->setError(error);

   // set content type
   pResponse->setContentType(targetFile.mimeContentType());

   // parse the range field
   std::string range = request.headerValue("Range");
   boost::regex re("bytes=(\\d*)\\-(\\d*)");
   boost::smatch match;
   if (boost::regex_match(range, match, re))
   {
      // specify partial content
      pResponse->setStatusCode(http::status::PartialContent);

      // determine the byte range
      const size_t kNone = -1;
      size_t begin = safe_convert::stringTo<size_t>(match[1], kNone);
      size_t end = safe_convert::stringTo<size_t>(match[2], kNone);
      size_t total = contents.length();

      if (end == kNone)
      {
         end = total-1;
      }
      if (begin == kNone)
      {
         begin = total - end;
         end = total-1;
      }

      // set the byte range
      pResponse->addHeader("Accept-Ranges", "bytes");
      boost::format fmt("bytes %1%-%2%/%3%");
      std::string range = boost::str(fmt % begin % end % contents.length());
      pResponse->addHeader("Content-Range", range);

      // always attempt gzip
      if (request.acceptsEncoding(http::kGzipEncoding))
         pResponse->setContentEncoding(http::kGzipEncoding);

      // set body
      if (begin == 0 && end == (contents.length()-1))
         pResponse->setBody(contents);
      else
         pResponse->setBody(contents.substr(begin, end-begin));
   }
   else
   {
      pResponse->setStatusCode(http::status::RangeNotSatisfiable);
      boost::format fmt("bytes */%1%");
      std::string range = boost::str(fmt % contents.length());
      pResponse->addHeader("Content-Range", range);
   }
}


bool hasKnitrVersion1()
{
   bool hasVersion = false;
   Error error = r::exec::RFunction(".rs.hasKnitrVersion1").call(&hasVersion);
   if (error)
      LOG_ERROR(error);
   return hasVersion;
}

bool knitSlides(const FilePath& slidesRmd, std::string* pErrMsg)
{
   // R binary
   FilePath rProgramPath;
   Error error = module_context::rScriptPath(&rProgramPath);
   if (error)
   {
      *pErrMsg = error.summary();
      return false;
   }

   // confirm correct version of knitr
   if (!hasKnitrVersion1())
   {
      *pErrMsg = "knitr version 1.0 or greater is required for presentations";
      return false;
   }

   // args
   std::vector<std::string> args;
   args.push_back("--silent");
   args.push_back("--no-save");
   args.push_back("--no-restore");
   args.push_back("-e");
   boost::format fmt("library(knitr); "
                     "opts_chunk$set(cache=TRUE,     "
                     "               results='hide', "
                     "               tidy=FALSE,     "
                     "               comment=NA);    "
                     "knit('%2%', encoding='%1%');");
   std::string encoding = projects::projectContext().defaultEncoding();
   std::string cmd = boost::str(fmt % encoding % slidesRmd.filename());
   args.push_back(cmd);

   // options
   core::system::ProcessOptions options;
   core::system::ProcessResult result;
   options.workingDir = slidesRmd.parent();

   // run knit
   error = core::system::runProgram(
            core::string_utils::utf8ToSystem(rProgramPath.absolutePath()),
            args,
            "",
            options,
            &result);
   if (error)
   {
      *pErrMsg = error.summary();
      return false;
   }
   else if (result.exitStatus != EXIT_SUCCESS)
   {
      *pErrMsg = "Error occurred during knit: " + result.stdErr;
      return false;
   }
   else
   {
      return true;
   }
}

void handlePresentationPaneRequest(const http::Request& request,
                                   http::Response* pResponse)
{
   // return not found if presentation isn't active
   if (!presentation::state::isActive())
   {
      pResponse->setError(http::status::NotFound, request.uri() + " not found");
      return;
   }

   // get the requested path
   std::string path = http::util::pathAfterPrefix(request, "/presentation/");

   // special handling for the root (process template)
   if (path.empty())
   {
      // look for slides.Rmd and knit it if we are in author mode
      FilePath presDir = presentation::state::directory();
      if (presentation::state::authorMode())
      {
         FilePath rmdFile = presDir.complete("slides.Rmd");
         if (rmdFile.exists())
         {
            std::string errMsg;
            if (!knitSlides(rmdFile, &errMsg))
            {
               pResponse->setError(http::status::InternalServerError,
                                   errMsg);
               return;
            }
         }
      }

      // look for slides.md
      FilePath slidesFile = presDir.complete("slides.md");
      if (!slidesFile.exists())
      {
         pResponse->setError(http::status::NotFound,
                             "slides.md file not found in " +
                             presentation::state::directory().absolutePath());
         return;
      }

      // parse the slides
      presentation::SlideDeck slideDeck;
      Error error = slideDeck.readSlides(slidesFile);
      if (error)
      {
         LOG_ERROR(error);
         pResponse->setError(http::status::InternalServerError,
                             error.summary());
         return;
      }

      // render the slides
      std::string slides, revealConfig, initCommands, slideCommands;
      error = presentation::renderSlides(slideDeck,
                                         &slides,
                                         &revealConfig,
                                         &initCommands,
                                         &slideCommands);
      if (error)
      {
         LOG_ERROR(error);
         pResponse->setError(http::status::InternalServerError,
                             error.summary());
         return;
      }

      // get user css if it exists
      std::string userSlidesCss;
      FilePath cssPath = presentation::state::directory().complete("slides.css");
      if (cssPath.exists())
      {
         userSlidesCss = "<link rel=\"stylesheet\" href=\"slides.css\">\n";
      }

      // build template variables
      std::map<std::string,std::string> vars;
      vars["title"] = slideDeck.title();
      vars["user_slides_css"] = userSlidesCss;
      vars["preamble"] = slideDeck.preamble();
      vars["slides"] = slides;
      vars["slide_commands"] = slideCommands;
      vars["slides_css"] =  resourceFiles().get("presentation/slides.css");
      vars["r_highlight"] = resourceFiles().get("r_highlight.html");
      vars["mathjax"] = mathjaxIfRequired(slides);
      vars["slides_js"] = resourceFiles().get("presentation/slides.js");
      vars["reveal_config"] = revealConfig;
      vars["init_commands"] = initCommands;

      // process the template
      pResponse->setNoCacheHeaders();
      pResponse->setBody(resourceFiles().get("presentation/slides.html"),
                         text::TemplateFilter(vars));
   }

   // special handling for reveal.js assets
   else if (boost::algorithm::starts_with(path, "revealjs/"))
   {
      path = http::util::pathAfterPrefix(request, "/presentation/revealjs/");
      FilePath filePath = presentationResourcesPath().complete("revealjs/" + path);
      pResponse->setFile(filePath, request);
   }

   // special handling for mathjax assets
   else if (boost::algorithm::starts_with(path, "mathjax/"))
   {
      FilePath filePath =
            session::options().mathjaxPath().parent().childPath(path);
      pResponse->setFile(filePath, request);
   }


   // serve the file back
   else
   {
      FilePath targetFile = presentation::state::directory().childPath(path);
      if (!request.headerValue("Range").empty())
      {
         handleRangeRequest(targetFile, request, pResponse);
      }
      else
      {
         // indicate that we accept byte range requests
         pResponse->addHeader("Accept-Ranges", "bytes");

         // return the file
         pResponse->setFile(targetFile, request);
      }
   }
}


// we save the most recent /help/presentation/&file=parameter so we
// can resolve relative file references against it. we do this
// separately from presentation::state::directory so that the help
// urls can be available within the help pane (and history)
// independent of the duration of the presentation tab
FilePath s_presentationHelpDir;


} // anonymous namespace

void handlePresentationHelpRequest(const core::http::Request& request,
                                   const std::string& jsCallbacks,
                                   core::http::Response* pResponse)
{
   // check if this is a root request
   std::string file = request.queryParamValue("file");
   if (!file.empty())
   {
      // ensure file exists
      FilePath filePath = module_context::resolveAliasedPath(file);
      if (!filePath.exists())
      {
         pResponse->setError(http::status::NotFound, request.uri());
         return;
      }

      // save the file's directory (for resolving other resources)
      s_presentationHelpDir = filePath.parent();


      // read in the file (process markdown)
      std::string helpDoc;
      Error error = markdown::markdownToHTML(filePath,
                                             markdown::Extensions(),
                                             markdown::HTMLOptions(),
                                             &helpDoc);
      if (error)
      {
         pResponse->setError(error);
         return;
      }

      // process the template
      std::map<std::string,std::string> vars;
      vars["title"] = html_utils::defaultTitle(helpDoc);
      vars["styles"] = resourceFiles().get("presentation/helpdoc.css");
      vars["r_highlight"] = resourceFiles().get("r_highlight.html");
      vars["mathjax"] = mathjaxIfRequired(helpDoc);
      vars["content"] = helpDoc;
      vars["js_callbacks"] = jsCallbacks;
      pResponse->setNoCacheHeaders();
      pResponse->setBody(resourceFiles().get("presentation/helpdoc.html"),
                         text::TemplateFilter(vars));
   }

   // it's a relative file reference
   else
   {
      // make sure the directory exists
      if (!s_presentationHelpDir.exists())
      {
         pResponse->setError(http::status::NotFound,
                             "Directory not found: " +
                             s_presentationHelpDir.absolutePath());
         return;
      }

      // resolve the file reference
      std::string path = http::util::pathAfterPrefix(request,
                                                     "/help/presentation/");

      // serve the file back
      pResponse->setFile(s_presentationHelpDir.complete(path), request);
   }
}



json::Value presentationStateAsJson()
{
   return presentation::state::asJson();
}

Error initialize()
{
   if (session::options().programMode() == kSessionProgramModeServer)
   {
      // register rs_showPresentation
      R_CallMethodDef methodDefShowPresentation;
      methodDefShowPresentation.name = "rs_showPresentation" ;
      methodDefShowPresentation.fun = (DL_FUNC) rs_showPresentation;
      methodDefShowPresentation.numArgs = 3;
      r::routines::addCallMethod(methodDefShowPresentation);

      using boost::bind;
      using namespace session::module_context;
      ExecBlock initBlock ;
      initBlock.addFunctions()
         (bind(registerUriHandler, "/presentation", handlePresentationPaneRequest))
         (bind(registerRpcMethod, "set_presentation_slide_index", setPresentationSlideIndex))
         (bind(registerRpcMethod, "close_presentation_pane", closePresentationPane))
         (bind(presentation::state::initialize))
         (bind(sourceModuleRFile, "SessionPresentation.R"));

      return initBlock.execute();
   }
   else
   {
      return Success();
   }
}

} // namespace presentation
} // namespace modules
} // namesapce session
