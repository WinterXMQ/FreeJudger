#include "Task.h"

#include "../judgerlib/taskmanager/TaskManager.h"
#include "../judgerlib/config/AppConfig.h"
#include "../judgerlib/filetool/FileTool.h"


#include "Compiler.h"
#include "Excuter.h"
#include "Matcher.h"

extern bool g_sigExit;

#pragma warning(push)
#pragma warning(disable:4996)

namespace IMUST
{
/* 延迟删除文件。
当某子进程尚未完全退出时，他占用的文件再次被打开或删除，都会失败。故作延迟，等待一段时间。
如果文件始终无法删除，将表示该子进程无法退出，这将是一个致命错误，评判线程应当结束。 */
bool safeRemoveFile(const OJString & file)
{
    OJChar_t buffer[128];

    for(OJInt32_t i=0; i<10; ++i)//尝试删除10次
    {
        if(FileTool::RemoveFile(file))
        {
            return true;
        }
        OJSleep(1000);

        OJSprintf(buffer, OJStr("safeRemoveFile '%s' faild with %d times. code:%d"), 
            file.c_str(), i+1, GetLastError());
        LoggerFactory::getLogger(LoggerId::AppInitLoggerId)->logError(buffer);
    }

    return false;
}

//获得文件扩展名
//TODO: 将语言用到的数字，用常量代替。
OJString getLanguageExt(OJInt32_t language)
{
    if(language == AppConfig::Language::C)
    {
        return OJStr("c");
    }
    else if(language == AppConfig::Language::Cxx)
    {
        return OJStr("cpp");
    }
    else if(language == AppConfig::Language::Java)
    {
        return OJStr("java");
    }

    return OJStr("unknown");
}

JudgeTask::JudgeTask(const TaskInputData & inputData) 
    : Input(inputData)
    , judgeID_(0)
{
    output_.Result = AppConfig::JudgeCode::SystemError;
    output_.PassRate = 0.0f;
    output_.RunTime = 0;
    output_.RunMemory = 0;
}

void JudgeTask::init(OJInt32_t judgeID)
{
    judgeID_ = judgeID;

    OJString fileExt = getLanguageExt(Input.Language);

    OJChar_t buffer[1024];

    OJSprintf(buffer, OJStr("work\\%d\\Main.%s"), judgeID_, fileExt.c_str());
    codeFile_ = buffer;
    
    FileTool::WriteFile(Input.UserCode, codeFile_);

    OJSprintf(buffer, OJStr("work\\%d\\Main.exe"), judgeID_);
    exeFile_ = buffer;

    OJSprintf(buffer, OJStr("work\\%d\\compile.txt"), judgeID_);
    compileFile_ = buffer;

    OJSprintf(buffer, OJStr("work\\%d\\output.txt"), judgeID_);
    userOutputFile_ = buffer;
}

bool JudgeTask::run()
{
    doRun();

    if(!doClean())
    {
        return false;//致命错误
    }

    return true;
}

void JudgeTask::doRun()
{
    ILogger *logger = LoggerFactory::getLogger(LoggerId::AppInitLoggerId);

    OJChar_t buf[256];

    OJSprintf(buf, OJStr("[JudgeTask] task %d"), Input.SolutionID);
    logger->logInfo(buf);

    //编译
    if(!compile())
    {
        return;
    }

    //搜索测试数据
    //TODO: 根据是否specialJudge，决定搜索.out还是.in文件。

    OJSprintf(buf, OJStr("/%d"), Input.ProblemID);
    OJString path = AppConfig::Path::TestDataPath;
    path += buf;

    logger->logTrace(OJString(OJStr("[JudgeTask] searche path: "))+path);

    FileTool::FileNameList fileList;
    FileTool::GetSpecificExtFiles(fileList, path, OJStr(".out"), true);

    OJUInt32_t testCount = fileList.size();
    if(testCount <= 0)
    {
        output_.Result = AppConfig::JudgeCode::SystemError;

        OJSprintf(buf, OJStr("[JudgeTask] not found test data for solution %d problem %d."),
            Input.SolutionID, Input.ProblemID);
        logger->logError(buf);
        return;
    }

    //测试多组数据
    OJUInt32_t accepted = 0;
    for(OJUInt32_t i=0; i<testCount; ++i)
    {
        answerOutputFile_ = fileList[i];
        answerInputFile_ = FileTool::RemoveFileExt(answerOutputFile_);
        answerInputFile_ += OJStr(".in");

        logger->logTrace(OJString(OJStr("[JudgeTask] input file: ")) + answerInputFile_);
        logger->logTrace(OJString(OJStr("[JudgeTask] output file: ")) + answerOutputFile_);

        if(!safeRemoveFile(userOutputFile_))
        {
            output_.Result = AppConfig::JudgeCode::SystemError;
            break;
        }

        if(!excute())
        {
            break;
        }
            
        if(!match())
        {
            break;
        }
        
        ++accepted;
    }

    output_.PassRate = float(accepted)/testCount;
}

bool JudgeTask::doClean()
{
    bool faild = false;

    if(!safeRemoveFile(codeFile_))
    {
        faild = true;
    }
    if(!safeRemoveFile(exeFile_))
    {
        faild = true;
    }
    if(!safeRemoveFile(userOutputFile_))
    {
        faild = true;
    }
    if(!safeRemoveFile(compileFile_))
    {
        faild = true;
    }

    if(faild)
    {
        output_.Result = AppConfig::JudgeCode::SystemError;
    }

    return !faild;
}

bool JudgeTask::compile()
{
    ILogger *logger = LoggerFactory::getLogger(LoggerId::AppInitLoggerId);
    logger->logTrace(OJStr("[JudgeTask] start compile..."));
    
    CompilerPtr compiler = CompilerFactory::create(Input.Language);
    compiler->run(codeFile_, exeFile_, compileFile_);

    if(compiler->isAccept())
    {
        output_.Result = AppConfig::JudgeCode::Accept;
    }
    else if(compiler->isSystemError())
    {
        output_.Result = AppConfig::JudgeCode::SystemError;
    }
    else if(compiler->isCompileError())
    {
        output_.Result = AppConfig::JudgeCode::CompileError;
        
        std::vector<OJChar_t> buffer;
        if(FileTool::ReadFile(buffer, compileFile_) && !buffer.empty())
        {
            output_.CompileError = &buffer[0];
        }
    }

    return compiler->isAccept();
}

bool JudgeTask::excute()
{
    ILogger *logger = LoggerFactory::getLogger(LoggerId::AppInitLoggerId);
    logger->logTrace(OJStr("[JudgeTask] start excute..."));

    if(!FileTool::IsFileExist(exeFile_))
    {
        logger->logError(OJStr("[JudgeTask] not found exe file!"));
        output_.Result = AppConfig::JudgeCode::SystemError;
        return false;
    }

    ExcuterPtr excuter = ExcuterFactory::create(Input.Language);
    excuter->run(exeFile_, answerInputFile_, userOutputFile_, Input.LimitTime, Input.LimitMemory);
    
    if(excuter->isAccept())
    {
        output_.Result = AppConfig::JudgeCode::Accept;
    }
    else if(excuter->isSystemError())
    {
        output_.Result = AppConfig::JudgeCode::SystemError;
    }
    else if(excuter->isOutputOutOfLimited())
    {
        output_.Result = AppConfig::JudgeCode::OutputLimited;
    }
    else if(excuter->isTimeOutOfLimited())
    {
        output_.Result = AppConfig::JudgeCode::TimeLimitExceed;
    }
    else if(excuter->isMemoryOutOfLimited())
    {
        output_.Result = AppConfig::JudgeCode::MemoryLimitExceed;
    }
    else if(excuter->isRuntimeError())
    {
        output_.Result = AppConfig::JudgeCode::RuntimeError;
    }

    output_.RunTime = excuter->getRunTime();
    output_.RunMemory = excuter->getRunMemory();

    return excuter->isAccept();
}

bool JudgeTask::match()
{
    ILogger *logger = LoggerFactory::getLogger(LoggerId::AppInitLoggerId);
    logger->logTrace(OJStr("[JudgeTask] start match..."));

    MatcherPtr matcher = MatcherFactory::create();
    matcher->run(answerOutputFile_, userOutputFile_);

    if(matcher->isAccept())
    {
        output_.Result = AppConfig::JudgeCode::Accept;
    }
    else if(matcher->isPresentError())
    {
        output_.Result = AppConfig::JudgeCode::PresentError;
    }
    else if(matcher->isWrongAnswer())
    {
        output_.Result = AppConfig::JudgeCode::WrongAnswer;
    }
    else if(matcher->isSystemError())
    {
        output_.Result = AppConfig::JudgeCode::SystemError;
    }

    return matcher->isAccept();
}



JudgeThread::JudgeThread(int id, IMUST::TaskManagerPtr working, IMUST::TaskManagerPtr finish)
    : id_(id)
    , workingTaskMgr_(working)
    , finisheTaskMgr_(finish)
{

}

void JudgeThread::operator()()
{
    ILogger *logger = LoggerFactory::getLogger(LoggerId::AppInitLoggerId);

    OJChar_t buffer[128];
    OJSprintf(buffer, OJStr("work/%d"), id_);
    FileTool::MakeDir(buffer);

    OJSprintf(buffer, OJStr("[JudgeThread][%d]start..."), id_);
    logger->logTrace(buffer);

    while (!g_sigExit)
    {

        IMUST::ITask* pTask = NULL;

        //从任务队列取任务
        workingTaskMgr_->lock();
        if(workingTaskMgr_->hasTask())
        {
            pTask = workingTaskMgr_->popTask();
        }
        workingTaskMgr_->unlock();

        if(!pTask)//没有任务
        {
            OJSleep(1000);
            continue;
        }

        pTask->init(id_);
        if(!pTask->run())
        {
            OJSprintf(buffer, OJStr("[JudgeThread][%d]System Error!Judge thread will exit!"), id_);
            logger->logError(buffer);
            break;
        }

        //添加到完成队列
        finisheTaskMgr_->lock();
        finisheTaskMgr_->addTask(pTask);
        finisheTaskMgr_->unlock();

        OJSleep(10);//防止线程过度繁忙
    }

    OJSprintf(buffer, OJStr("[JudgeThread][%d]end."), id_);
    logger->logTrace(buffer);

}

ITask* JudgeTaskFactory::create(const TaskInputData & input)
{
    return new JudgeTask(input);
}

void JudgeTaskFactory::destroy(ITask* pTask)
{
    delete pTask;
}


JudgeDBRunThread::JudgeDBRunThread(IMUST::DBManagerPtr dbm)
    : dbm_(dbm)
{
}

void JudgeDBRunThread::operator()()
{
    IMUST::ILogger *logger = IMUST::LoggerFactory::getLogger(IMUST::LoggerId::AppInitLoggerId);
    logger->logTrace(GetOJString("[DBThread] thread start..."));

    while(!g_sigExit)
    {
        if(!dbm_->run())
        {
            logger->logError(GetOJString("[DBThread] thread was dead!"));
            break;
        }
        OJSleep(100);
    }

    logger->logTrace(GetOJString("[DBThread] thread end."));
}

}   // namespace IMUST

#pragma warning(pop)