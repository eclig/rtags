/* This file is part of RTags.

RTags is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTags is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#include "Clang.h"
#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS
#include <clang/Basic/Version.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/DataRecursiveASTVisitor.h>

class RTagsCompilationDatabase : public clang::tooling::CompilationDatabase
{
public:
    RTagsCompilationDatabase(const Source &source, const String &unsaved = String())
        : mSource(source), mUnsaved(unsaved)
    {
        mCommand.Directory = source.pwd;
        const unsigned int commandLineFlags = (Source::FilterBlacklist
                                               | Source::IncludeDefines
                                               | Source::IncludeIncludepaths
                                               | Source::IncludeSourceFile
                                               | Source::IncludeLibClangOptions);
        const List<String> args = source.toCommandLine(commandLineFlags);
        mCommand.CommandLine.resize(args.size());
        int i = 0;

        for (const auto &str : args) {
            mCommand.CommandLine[i++] = str;
        }
        if (!unsaved.isEmpty()) {
            mCommand.MappedSources.push_back(std::make_pair(source.sourceFile(), unsaved));
        }
    }

    virtual std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef file) const
    {
        Path path(file.data(), file.size());
        if (path.isSameFile(mSource.sourceFile()))
            return getAllCompileCommands();
        return std::vector<clang::tooling::CompileCommand>();
    }

    virtual std::vector<std::string> getAllFiles() const
    {
        return std::vector<std::string>(1, mSource.sourceFile());
    }

    virtual std::vector<clang::tooling::CompileCommand> getAllCompileCommands() const
    {
        return std::vector<clang::tooling::CompileCommand>(1, mCommand);
    }
private:
    clang::tooling::CompileCommand mCommand;
    const Source mSource;
    const String mUnsaved;
};

class RTagsASTConsumer : public clang::ASTConsumer, public clang::DataRecursiveASTVisitor<RTagsASTConsumer>
{
    typedef clang::DataRecursiveASTVisitor<RTagsASTConsumer> base;
public:
    RTagsASTConsumer(Clang *clang)
        : mClang(clang), mAborted(false)
    {}

    void HandleTranslationUnit(clang::ASTContext &Context) override {
        clang::TranslationUnitDecl *D = Context.getTranslationUnitDecl();
        TraverseDecl(D);
    }

    bool shouldWalkTypesOfTypeLocs() const { return true; } // ### ???

    bool TraverseDecl(clang::Decl *d) {
        if (mAborted)
            return true;
        if (d) {
            error() << getName(d);
            switch (mClang->visit(d)) {
            case Clang::Abort:
                mAborted = true;
                return true;
            case Clang::SkipChildren:
                return true;
            case Clang::RecurseChildren:
                break;
            }
            // bodl ShowColors = Out.has_colors();
            // if (ShowColors)
            //     Out.changeColor(raw_ostream::BLUE);
            // Out << ((Dump || DumpLookups) ? "Dumping " : "Printing ") << getName(D)
            //     << ":\n";
            // if (ShowColors)
            //     Out.resetColor();
            // print(D);
            // Out << "\n";
            // Don't traverse child nodes to avoid output duplication.
            // return true;

        }
        return base::TraverseDecl(d);
    }

private:
    std::string getName(clang::Decl *D) {
        if (clang::isa<clang::NamedDecl>(D))
            return clang::cast<clang::NamedDecl>(D)->getQualifiedNameAsString();
        return "";
    }
    // void print(clang::Decl *D) {
        // if (DumpLookups) {
        //     if (clang::DeclContext *DC = clang::dyn_cast<clang::DeclContext>(D)) {
        //         if (DC == DC->getPrimaryContext())
        //             DC->dumpLookups(Out, Dump);
        //         else
        //             Out << "Lookup map is in primary DeclContext "
        //                 << DC->getPrimaryContext() << "\n";
        //     } else
        //         Out << "Not a DeclContext\n";
        // } else if (Dump)
        //     D->dump(Out);
        // else
        //     D->print(Out, /*Indentation=*/0, /*PrintInstantiation=*/true);
    // }
    Clang *mClang;
    bool mAborted;
};

class RTagsFrontendAction : public clang::ASTFrontendAction
{
public:
    RTagsFrontendAction(Clang *clang)
        : mClang(clang)
    {}
#if CLANG_VERSION_MAJOR > 3 || (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 6)
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef InFile) override
    {
        return std::unique_ptr<clang::ASTConsumer>(new RTagsASTConsumer(mClang));
    }
#else
    clang::ASTConsumer *CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef InFile) override
    {
        return new RTagsASTConsumer(mClang);
    }
#endif
private:
    Clang *mClang;
};

class RTagsFrontendActionFactory : public clang::tooling::FrontendActionFactory
{
public:
    RTagsFrontendActionFactory(Clang *clang)
        : mClang(clang)
    {}
    virtual clang::FrontendAction *create()
    {
        return new RTagsFrontendAction(mClang);
    }
private:
    Clang *mClang;
};

bool Clang::index(const Source &source, const String &unsaved)
{
    RTagsCompilationDatabase compilationDatabase(source);
    clang::tooling::ClangTool tool(compilationDatabase, compilationDatabase.getAllFiles());
    RTagsFrontendActionFactory factory(this);
    tool.run(&factory);
    return true;
}