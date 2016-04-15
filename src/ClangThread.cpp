/* This file is part of RTags (http://rtags.net).

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

#include "ClangThread.h"
#include "rct/Connection.h"

#include "RTags.h"
#include "Server.h"

struct Dep : public DependencyNode
{
    Dep(uint32_t f)
        : DependencyNode(f)
    {}
    Hash<uint32_t, Map<Location, Location> > references;
};

ClangThread::ClangThread(const std::shared_ptr<QueryMessage> &queryMessage,
                         const Source &source, const std::shared_ptr<Connection> &conn)
    : Thread(), mQueryMessage(queryMessage), mSource(source), mConnection(conn),
      mIndentLevel(0), mAborted(false)
{
    setAutoDelete(true);
}

static const CXSourceLocation nullLocation = clang_getNullLocation();
static const CXCursor nullCursor = clang_getNullCursor();

CXChildVisitResult ClangThread::visitor(CXCursor cursor, CXCursor, CXClientData userData)
{
    ClangThread *that = reinterpret_cast<ClangThread*>(userData);
    assert(that);
    return that->visit(cursor);
}

CXChildVisitResult ClangThread::visit(const CXCursor &cursor)
{
    if (isAborted())
        return CXChildVisit_Break;
    const Location location = createLocation(cursor);
    if (!location.isNull()) {
        if (mQueryMessage->flags() & QueryMessage::DumpCheckIncludes) {
            checkIncludes(location, cursor);
            return CXChildVisit_Recurse;
        } else if (mQueryMessage->flags() & QueryMessage::JSON) {
            visitAST(cursor, location);
            return CXChildVisit_Recurse;
        } else {
            Flags<Location::ToStringFlag> locationFlags;
            if (mQueryMessage->flags() & QueryMessage::NoColor)
                locationFlags |= Location::NoColor;

            CXSourceRange range = clang_getCursorExtent(cursor);
            CXSourceLocation rangeEnd = clang_getRangeEnd(range);
            unsigned int endLine, endColumn;
            clang_getPresumedLocation(rangeEnd, 0, &endLine, &endColumn);
            if (!(mQueryMessage->flags() & QueryMessage::DumpIncludeHeaders) && location.fileId() != mSource.fileId) {
                return CXChildVisit_Continue;
            }

            String message;
            message.reserve(256);

            if (!(mQueryMessage->flags() & QueryMessage::NoContext)) {
                message = location.context(locationFlags, &mContextCache);
            }

            if (endLine == location.line()) {
                message += String::format<32>(" // %d-%d, %d: ", location.column(), endColumn, mIndentLevel);
            } else {
                message += String::format<32>(" // %d-%d:%d, %d: ", location.column(), endLine, endColumn, mIndentLevel);
            }
            message += RTags::cursorToString(cursor, RTags::AllCursorToStringFlags);
            message.append(" " + RTags::typeName(cursor));;
            if (clang_getCursorKind(cursor) == CXCursor_VarDecl) {
                const std::shared_ptr<RTags::Auto> autoResolved = RTags::resolveAuto(cursor);
                if (autoResolved && !clang_equalCursors(autoResolved->cursor, nullCursor)) {
                    message += "auto resolves to " + RTags::cursorToString(autoResolved->cursor, RTags::AllCursorToStringFlags);
                }
            }
            CXCursor ref = clang_getCursorReferenced(cursor);
            if (clang_equalCursors(ref, cursor)) {
                message.append("refs self");
            } else if (!clang_equalCursors(ref, nullCursor)) {
                message.append("refs ");
                message.append(RTags::cursorToString(ref, RTags::AllCursorToStringFlags));
            }

            CXCursor canonical = clang_getCanonicalCursor(cursor);
            if (!clang_equalCursors(canonical, cursor) && !clang_equalCursors(canonical, nullCursor)) {
                message.append("canonical ");
                message.append(RTags::cursorToString(canonical, RTags::AllCursorToStringFlags));
            }

            CXCursor specialized = clang_getSpecializedCursorTemplate(cursor);
            if (!clang_equalCursors(specialized, cursor) && !clang_equalCursors(specialized, nullCursor)) {
                message.append("specialized ");
                message.append(RTags::cursorToString(specialized, RTags::AllCursorToStringFlags));
            }

            writeToConnetion(message);
        }
    }
    ++mIndentLevel;
    clang_visitChildren(cursor, ClangThread::visitor, this);
    if (isAborted())
        return CXChildVisit_Break;
    --mIndentLevel;
    return CXChildVisit_Continue;
}

void ClangThread::run()
{
    const auto key = mConnection->disconnected().connect([this](const std::shared_ptr<Connection> &) { abort(); });

    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit translationUnit = 0;
    String clangLine;
    RTags::parseTranslationUnit(mSource.sourceFile(), mSource.toCommandLine(Source::Default), translationUnit,
                                index, 0, 0, CXTranslationUnit_DetailedPreprocessingRecord, &clangLine);
    if (mQueryMessage->type() == QueryMessage::DumpFile && !(mQueryMessage->flags() & QueryMessage::DumpCheckIncludes))
        writeToConnetion(String::format<128>("Indexed: %s => %s", clangLine.constData(), translationUnit ? "success" : "failure"));
    if (translationUnit) {
        clang_visitChildren(clang_getTranslationUnitCursor(translationUnit), ClangThread::visitor, this);
        if (mQueryMessage->flags() &  QueryMessage::JSON) {
            dumpJSON(translationUnit);
        } else if (mQueryMessage->flags() & QueryMessage::DumpCheckIncludes) {
            checkIncludes();
        }
    } else if (mQueryMessage->flags() & QueryMessage::JSON) {
        writeToConnetion(String::format<1024>("{ \"file\": \"%s\", \"commandLine\": \"%s\", \"success\": false }",
                                              mSource.sourceFile().constData(),
                                              String::join(mSource.toCommandLine(Source::Default), ' ').constData()));
    }

    if (translationUnit)
        clang_disposeTranslationUnit(translationUnit);

    clang_disposeIndex(index);

    mConnection->disconnected().disconnect(key);
    std::weak_ptr<Connection> conn = mConnection;
    EventLoop::mainEventLoop()->callLater([conn]() {
            if (auto c = conn.lock())
                c->finish();
        });
}

void ClangThread::writeToConnetion(const String &message)
{
    std::weak_ptr<Connection> conn = mConnection;
    EventLoop::mainEventLoop()->callLater([conn, message]() {
            if (auto c = conn.lock()) {
                c->write(message);
            }
        });
}

void ClangThread::handleInclude(Location loc, const CXCursor &cursor)
{
    CXFile includedFile = clang_getIncludedFile(cursor);
    if (includedFile) {
        CXStringScope fn = clang_getFileName(includedFile);
        const char *cstr = clang_getCString(fn);
        if (!cstr) {
            clang_disposeString(fn);
            return;
        }
        const Path p = Path::resolved(cstr);
        clang_disposeString(fn);
        const uint32_t fileId = Location::insertFile(p);
        Dep *&source = mDependencies[loc.fileId()];
        if (!source)
            source = new Dep(loc.fileId());
        Dep *&include = mDependencies[fileId];
        if (!include)
            include = new Dep(fileId);
        source->include(include);
    }
}

void ClangThread::handleReference(Location loc, const CXCursor &ref)
{
    if (clang_getCursorKind(ref) == CXCursor_Namespace)
        return;
    const Location refLoc = createLocation(ref);
    if (refLoc.isNull() || refLoc.fileId() == loc.fileId())
        return;

    Dep *dep = mDependencies[loc.fileId()];
    assert(dep);
    Dep *refDep = mDependencies[refLoc.fileId()];
    assert(refDep);
    auto &refs = dep->references[refDep->fileId];
    refs[loc] = refLoc;
}

void ClangThread::checkIncludes(Location location, const CXCursor &cursor)
{
    if (clang_getCursorKind(cursor) == CXCursor_InclusionDirective) {
        handleInclude(location, cursor);
    } else {
        const CXCursor ref = clang_getCursorReferenced(cursor);
        if (!clang_equalCursors(cursor, nullCursor) && !clang_equalCursors(cursor, ref)) {
            handleReference(location, ref);
        }
    }
}

static bool validateHasInclude(uint32_t ref, const Dep *cur, Set<uint32_t> &seen)
{
    assert(ref);
    assert(cur);
    if (cur->includes.contains(ref))
        return true;
    if (!seen.insert(ref))
        return false;
    for (const auto &pair : cur->includes) {
        if (validateHasInclude(ref, static_cast<const Dep*>(pair.second), seen))
            return true;
    }
    return false;
}

static bool validateNeedsInclude(const Dep *source, const Dep *header, Set<uint32_t> &seen)
{
    if (!seen.insert(header->fileId)) {
        // error() << "already seen" << Location::path(source->fileId);
        return false;
    }
    if (source->references.contains(header->fileId)) {
        // error() << "Got ref" << Location::path(header->fileId);
        return true;
    }
    for (const auto &child : header->includes) {
        // error() << "Checking child" << Location::path(child.second->fileId);
        if (validateNeedsInclude(source, static_cast<const Dep*>(child.second), seen)) {
            return true;
        }
    }

    // error() << "Checking" << Location::path(source->fileId) << "doesn't seem to need" << Location::path(header->fileId) << depth;
    return false;
}

void ClangThread::checkIncludes()
{
    for (const auto &it : mDependencies) {
        const Path path = Location::path(it.first);
        if (path.isSystem())
            continue;

        for (const auto &dep  : it.second->includes) {
            Set<uint32_t> seen;
            if (!validateNeedsInclude(it.second, static_cast<Dep*>(dep.second), seen)) {
                writeToConnetion(String::format<128>("%s includes %s for no reason",
                                                     path.constData(),
                                                     Location::path(dep.second->fileId).constData()));
            }
        }

        for (const auto &ref : it.second->references) {
            const Path refPath = Location::path(ref.first);
            if (refPath.startsWith("/usr/include/sys/_types/_") || refPath.startsWith("/usr/include/_types/_"))
                continue;
            Set<uint32_t> seen;
            if (!validateHasInclude(ref.first, it.second, seen)) {
                List<String> reasons;
                for (const auto &r : ref.second) {
                    String reason;
                    Log log(&reason);
                    log << r.first << "=>" << r.second;
                    reasons << reason;
                }
                writeToConnetion(String::format<128>("%s should include %s (%s)",
                                                     Location::path(it.first).constData(),
                                                     Location::path(ref.first).constData(),
                                                     String::join(reasons, " ").constData()));
                // for (const auto &incs : mDependencies[ref.first]->dependents) {
                //     writeToConnetion(String::format<128>("GOT INCLUDER %s:%d", Location::path(incs.first).constData(),
                //                                          incs.first));
                // }
            }
        }
    }

    for (auto it : mDependencies) {
        delete it.second;
    }
}

ClangThread::Cursor *ClangThread::visitAST(const CXCursor &cursor, Location location)
{
    auto recurse = [&cursor, this](const CXCursor &other) -> Cursor * {
        if (!clang_equalCursors(cursor, other) && !clang_equalCursors(cursor, nullCursor))
            return visitAST(other);
        return 0;
    };
    error() << "visiting" << cursor << RTags::eatString(clang_getCursorUSR(cursor));
    const CXCursorKind kind = clang_getCursorKind(cursor);
    if (clang_isInvalid(kind)) {
        return 0;
    }
    const String usr = RTags::eatString(clang_getCursorUSR(cursor));
    if (!usr.isEmpty()) {
        if (Cursor *ret = mCursorsByUsr.value(usr)) {
            return ret;
        }
    }
    if (location.isNull()) {
        location = createLocation(cursor);
        if (location.isNull()) {
            return 0;
        }
    }

    error() << "GOT DUDE" << location;
    static int max = 100;
    if (--max == 0)
        exit(0);

    std::shared_ptr<Cursor> c(new Cursor);
    c->location = location;
    const CXSourceRange range = clang_getCursorExtent(cursor);
    c->rangeStart = createLocation(clang_getRangeStart(range));
    c->rangeEnd = createLocation(clang_getRangeEnd(range));
    c->usr = usr;
    if (!c->usr.isEmpty())
        mCursorsByUsr[c->usr] = c.get();
    c->kind << clang_getCursorKindSpelling(kind);
    Log(&c->linkage) << clang_getCursorLinkage(cursor);
    Log(&c->availability) << clang_getCursorAvailability(cursor);
    c->spelling << clang_getCursorSpelling(cursor);
    c->displayName << clang_getCursorDisplayName(cursor);
    c->mangledName << clang_Cursor_getMangling(cursor);
    c->templateCursorKind << clang_getCursorKindSpelling(clang_getTemplateCursorKind(cursor));
    c->referenced = recurse(clang_getCursorReferenced(cursor));
    c->canonical = recurse(clang_getCanonicalCursor(cursor));
    c->lexicalParent = recurse(clang_getCursorLexicalParent(cursor));
    c->semanticParent = recurse(clang_getCursorSemanticParent(cursor));
    c->specializedCursorTemplate = recurse(clang_getSpecializedCursorTemplate(cursor));
    if (clang_isCursorDefinition(cursor)) {
        c->flags |= Cursor::Definition;
    } else {
        c->definition = recurse(clang_getCursorDefinition(cursor));
    }
    {
        CXCursor *overridden = 0;
        unsigned count;
        clang_getOverriddenCursors(cursor, &overridden, &count);
        for (unsigned i=0; i<count; ++i) {
            if (Cursor *cc = recurse(overridden[i]))
                c->overridden.append(cc);
        }
        clang_disposeOverriddenCursors(overridden);
    }
    c->bitFieldWidth = clang_getFieldDeclBitWidth(cursor);
    {
        const int count = clang_Cursor_getNumArguments(cursor);
        for (int i=0; i<count; ++i) {
            if (Cursor *cc = recurse(clang_Cursor_getArgument(cursor, i))) {
                c->arguments.append(cc);
            }
        }
    }
    {
        const int count = clang_Cursor_getNumTemplateArguments(cursor);
        if (count > 0) {
            c->templateArguments.resize(count);
            for (int i=0; i<count; ++i) {
                Log(&c->templateArguments[i].kind) << clang_Cursor_getTemplateArgumentKind(cursor, i);
                c->templateArguments[i].value = clang_Cursor_getTemplateArgumentValue(cursor, i);
                c->templateArguments[i].unsignedValue = clang_Cursor_getTemplateArgumentUnsignedValue(cursor, i);
                c->templateArguments[i].type = createType(clang_Cursor_getTemplateArgumentType(cursor, i));
            }
        }
    }
    c->type = createType(clang_getCursorType(cursor));
    c->receiverType = createType(clang_Cursor_getReceiverType(cursor));
    c->typedefUnderlyingType = createType(clang_getTypedefDeclUnderlyingType(cursor));
    c->enumDeclIntegerType = createType(clang_getEnumDeclIntegerType(cursor));
    c->resultType = createType(clang_getCursorResultType(cursor));
    if (clang_Cursor_isBitField(cursor))
        c->flags |= Cursor::BitField;
    if (clang_isVirtualBase(cursor))
        c->flags |= Cursor::VirtualBase;
    if (clang_Cursor_isDynamicCall(cursor))
        c->flags |= Cursor::DynamicCall;
    if (clang_Cursor_isVariadic(cursor))
        c->flags |= Cursor::Variadic;
    if (clang_CXXMethod_isVirtual(cursor))
        c->flags |= Cursor::Virtual;
    if (clang_CXXMethod_isPureVirtual(cursor))
        c->flags |= Cursor::PureVirtual;
    if (clang_CXXMethod_isStatic(cursor))
        c->flags |= Cursor::Static;
    if (clang_CXXMethod_isConst(cursor))
        c->flags |= Cursor::Const;

    CXFile includedFile = clang_getIncludedFile(cursor);
    if (includedFile) {
        CXStringScope fn = clang_getFileName(includedFile);
        const char *cstr = clang_getCString(fn);
        if (cstr)
            c->includedFile = Path::resolved(cstr);
        clang_disposeString(fn);
    }

    c->id = mCursors.size();
    mCursors.append(c.get());
    return c.get();
}
ClangThread::Type *ClangThread::createType(const CXType &type)
{
    const String spelling = RTags::eatString(clang_getTypeSpelling(type));
    if (spelling.isEmpty())
        return 0;
    Type *&t = mTypesBySpelling[spelling];
    if (t)
        return t;
    t = new Type;
    t->id = mTypes.size();
    mTypes.append(std::shared_ptr<Type>(t));
    t->spelling = spelling;
    t->kind << clang_getTypeKindSpelling(type.kind);
    t->typeDeclaration = visitAST(clang_getTypeDeclaration(type));
    t->numElements = clang_getNumElements(type);
   t->align = clang_Type_getAlignOf(type);
    t->sizeOf = clang_Type_getSizeOf(type);
    t->numElements = clang_getNumElements(type);
    t->arraySize = clang_getArraySize(type);
    Log(&t->callingConvention) << clang_getFunctionTypeCallingConv(type);
    Log(&t->callingConvention) << clang_getFunctionTypeCallingConv(type);
    if (clang_isConstQualifiedType(type))
        t->flags |= Type::ConstQualified;
    if (clang_isVolatileQualifiedType(type))
        t->flags |= Type::VolatileQualified;
    if (clang_isRestrictQualifiedType(type))
        t->flags |= Type::RestrictQualified;
    if (clang_isFunctionTypeVariadic(type))
        t->flags |= Type::Variadic;
    if (clang_isPODType(type))
        t->flags |= Type::POD;

    t->pointeeType = createType(clang_getPointeeType(type));
    t->elementType = createType(clang_getElementType(type));
    t->canonicalType = createType(clang_getCanonicalType(type));
    t->resultType = createType(clang_getResultType(type));
    t->arrayElementType = createType(clang_getArrayElementType(type));
    t->classType = createType(clang_Type_getClassType(type));
    {
        const int count = clang_getNumArgTypes(type);
        for (int i=0; i<count; ++i) {
            if (Type *tt = createType(clang_getArgType(type, i))) {
                t->arguments.append(tt);
            }
        }
    }
    {
        const int count = clang_Type_getNumTemplateArguments(type);
        for (int i=0; i<count; ++i) {
            if (Type *tt = createType(clang_Type_getTemplateArgumentAsType(type, i))) {
                t->templateArguments.append(tt);
            }
        }
    }
    switch (clang_Type_getCXXRefQualifier(type)) {
    case CXRefQualifier_None:
        break;
    case CXRefQualifier_LValue:
        t->flags |= Type::LValue;
        break;
    case CXRefQualifier_RValue:
        t->flags |= Type::RValue;
        break;
    }
    return t;
}

void ClangThread::dumpJSON(CXTranslationUnit unit)
{
    Value out;
    out["file"] = mSource.sourceFile();
    out["commandLine"] = mSource.toCommandLine(Source::Default);
    Set<uint32_t> files;
    Value &cursors = out["cursors"];
    Value &types = out["types"];
    Value &skippedRanges = out["skippedRanges"];
    Value &diagnostics = out["diagnostics"];
    for (const auto &t : mTypes) {
        types.push_back(t->toValue());
    }
    for (const auto &c : mCursors) {
        files.insert(c->location.fileId());
        cursors.push_back(c->toValue());
    }
#if CINDEX_VERSION >= CINDEX_VERSION_ENCODE(0, 21)
    for (uint32_t fileId : files) {
        const Path path = Location::path(fileId);
        const CXFile file = clang_getFile(unit, path.constData());
        if (!file)
            continue;
        if (CXSourceRangeList *skipped = clang_getSkippedRanges(unit, file)) {
            const unsigned int count = skipped->count;
            assert(count);
            Value &skippedFile = skippedRanges[path];
            for (unsigned int i=0; i<count; ++i) {
                const CXSourceLocation start = clang_getRangeStart(skipped->ranges[i]);
                const CXSourceLocation end = clang_getRangeEnd(skipped->ranges[i]);
                unsigned int startLine, startColumn, startOffset, endLine, endColumn, endOffset;
                clang_getSpellingLocation(start, 0, &startLine, &startColumn, &startOffset);
                clang_getSpellingLocation(end, 0, &endLine, &endColumn, &endOffset);
                Value range;
                range["startLine"] = startLine;
                range["startColumn"] = startLine;
                range["startOffset"] = startLine;
                range["endLine"] = endLine;
                range["endColumn"] = endLine;
                range["endOffset"] = endLine;
                skippedFile.push_back(range);
            }

            clang_disposeSourceRangeList(skipped);
        }
    }
#endif
    {
        const unsigned int diagnosticCount = clang_getNumDiagnostics(unit);

        for (unsigned int i=0; i<diagnosticCount; ++i) {
            CXDiagnostic diagnostic = clang_getDiagnostic(unit, i);
            diagnostics.push_back(diagnosticToValue(diagnostic));
            clang_disposeDiagnostic(diagnostic);
        }
    }
    const String json = out.toJSON();
    writeToConnetion(json);
}

Value ClangThread::Cursor::toValue() const
{
    Value ret;
    ret["id"] = id;
    ret["location"] = locationToValue(location);
    // error() << "shit at" << location << kind << rangeStart << rangeEnd;
    // assert(rangeStart.isNull() == rangeEnd.isNull());
    if (!rangeStart.isNull())
        ret["range"] = rangeToValue(rangeStart, rangeEnd);
    if (!includedFile.isEmpty())
        ret["includedFile"] = includedFile;
    ret["usr"] = usr;
    ret["kind"] = kind;
    ret["linkage"] = linkage;
    ret["availability"] = availability;
    ret["spelling"] = spelling;
    ret["displayName"] = displayName;
    ret["mangledName"] = mangledName;
    ret["templateCursorKind"] = templateCursorKind;

    auto addLink = [&ret](const char *name, Link *l) {
        if (l)
            ret[name] = l->id;
    };

    addLink("referenced", referenced);
    addLink("lexicalParent", lexicalParent);
    addLink("semanticParent", semanticParent);
    addLink("canonical", canonical);
    addLink("definition", definition);

    auto addLinkList = [&ret](const char *name, const List<Link*> &list) {
        if (list.isEmpty())
            return;
        Value &ref = ret[name];
        for (Link *c : list) {
            ref.push_back(c->id);
        }
    };
    addLinkList("overridden", overridden);
    addLinkList("arguments", arguments);
    addLinkList("overloadedDecls", overloadedDecls);
    if (bitFieldWidth > 0)
        ret["bitFieldWidth"] = bitFieldWidth;
    if (!templateArguments.isEmpty()) {
        for (const TemplateArgument &arg : templateArguments) {
            Value a;
            a["kind"] = arg.kind;
            a["value"] = arg.value;
            a["unsignedValue"] = arg.unsignedValue;
            if (arg.type) {
                a["type"] = arg.type->id;
            }
        }
    }

    addLink("type", type);
    addLink("receiverType", receiverType);
    addLink("typedefUnderlyingType", typedefUnderlyingType);
    addLink("enumDeclIntegerType", enumDeclIntegerType);
    addLink("resultType", resultType);

    auto addFlag = [&ret, this](const char *name, Flag flag) {
        if (flags & flag)
            ret[name] = true;
    };
    addFlag("BitField", BitField);
    addFlag("VirtualBase", VirtualBase);
    addFlag("Definition", Definition);
    addFlag("DynamicCall", DynamicCall);
    addFlag("Variadic", Variadic);
    addFlag("PureVirtual", PureVirtual);
    addFlag("Virtual", Virtual);
    addFlag("Static", Static);
    addFlag("Const", Const);
    return ret;
}

Value ClangThread::Type::toValue() const
{
    Value ret;
    ret["id"] = id;
    ret["spelling"] = spelling;
    ret["kind"] = kind;
    ret["element"] = element;
    ret["referenceType"] = referenceType;
    ret["callingConvention"] = callingConvention;

    auto addLink = [&ret](const char *name, Link *l) {
        if (l)
            ret[name] = l->id;
    };
    addLink("canonicalType", canonicalType);
    addLink("pointeeType", pointeeType);
    addLink("resultType", resultType);
    addLink("elementType", elementType);
    addLink("arrayElementType", arrayElementType);
    addLink("classType", classType);
    addLink("typeDeclaration", typeDeclaration);

    auto addLinkList = [&ret](const char *name, const List<Link*> &list) {
        if (list.isEmpty())
            return;
        Value &ref = ret[name];
        for (Link *c : list) {
            ref.push_back(c->id);
        }
    };
    addLinkList("arguments", arguments);
    addLinkList("templateArguments", templateArguments);

    auto addFlag = [&ret, this](const char *name, Flag flag) {
        if (flags & flag)
            ret[name] = true;
    };

    addFlag("ConstQualified", ConstQualified);
    addFlag("VolatileQualified", VolatileQualified);
    addFlag("RestrictQualified", RestrictQualified);
    addFlag("Variadic", Variadic);
    addFlag("RValue", RValue);
    addFlag("LValue", LValue);
    addFlag("POD", POD);

    auto addLongLong = [&ret](const char *name, long long val) {
        if (val > 0)
            ret[name] = val;
    };
    addLongLong("numElements", numElements);
    addLongLong("arraySize", arraySize);
    addLongLong("align", align);
    addLongLong("sizeOf", sizeOf);

    return ret;
}

Value ClangThread::diagnosticToValue(CXDiagnostic diagnostic)
{
    Value ret;
    String severity;
    Log(&severity) << clang_getDiagnosticSeverity(diagnostic);
    ret["severity"] = severity;
    String spelling;
    spelling << clang_getDiagnosticSpelling(diagnostic);
    ret["spelling"] = spelling;
    ret["location"] = locationToValue(createLocation(clang_getDiagnosticLocation(diagnostic)));
    CXString disable;
    ret["option"] = RTags::eatString(clang_getDiagnosticOption(diagnostic, &disable));
    ret["disable"] = RTags::eatString(disable);
    ret["category"] = RTags::eatString(clang_getDiagnosticCategoryText(diagnostic));
    const unsigned rangeCount = clang_getDiagnosticNumRanges(diagnostic);
    if (rangeCount) {
        Value &ranges = ret["ranges"];
        for (unsigned i=0; i<rangeCount; ++i) {
            ranges.push_back(rangeToValue(clang_getDiagnosticRange(diagnostic, i)));
        }
    }

    const unsigned fixitCount = clang_getDiagnosticNumFixIts(diagnostic);
    if (fixitCount) {
        Value &fixits = ret["fixits"];
        for (unsigned i=0; i<rangeCount; ++i) {
            Value fixit;
            CXSourceRange range;
            fixit["text"] = RTags::eatString(clang_getDiagnosticFixIt(diagnostic, i, &range));
            if (!clang_Range_isNull(range)) {
                fixit["replaceRange"] = rangeToValue(range);
            }
            fixits.push_back(fixit);
        }
    }

    CXDiagnosticSet children = clang_getChildDiagnostics(diagnostic);
    const unsigned diagnosticCount = clang_getNumDiagnosticsInSet(children);
    if (diagnosticCount) {
        Value &c = ret["children"];
        for (unsigned i=0; i<diagnosticCount; ++i) {
            CXDiagnostic child = clang_getDiagnosticInSet(children, i);
            c.push_back(diagnosticToValue(child));
            clang_disposeDiagnostic(child);
        }
    }
    return ret;
}

Value ClangThread::locationToValue(Location location)
{
    assert(!location.isNull());
    Value ret;
    ret["file"] = location.path();
    ret["line"] = location.line();
    ret["column"] = location.column();
    ret["location"] = location.toString(Location::AbsolutePath|Location::NoColor);
    return ret;
}

Value ClangThread::rangeToValue(CXSourceRange range)
{
    assert(!clang_Range_isNull(range));
    return rangeToValue(createLocation(clang_getRangeStart(range)), createLocation(clang_getRangeEnd(range)));
}

Value ClangThread::rangeToValue(Location start, Location end)
{
    Value v;
    v["start"] = locationToValue(start);
    v["end"] = locationToValue(end);
    return v;
}
