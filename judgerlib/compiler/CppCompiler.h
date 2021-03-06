#pragma once
#include "compiler.h"

namespace IMUST
{
class CppCompiler : public ICompiler
{
public:
    CppCompiler(void);

    virtual ~CppCompiler(void);

    virtual bool CppCompiler::run(
        const OJString & codeFile,
        const OJString & exeFile,
        const OJString & compileFile);
};

}// namespace IMUST