#pragma once
#include <initializer_list>
#include <string>
#include <string_view>
#include "os.h"

namespace ccc {

enum class Msg : unsigned {
    NoYesKeys = 0x2328,
    IncorrectParameter = 0x2329,
    SyntaxIncorrect = 0x232A,
    PressAnyKey = 0x232B,
    DeletePrompt = 0x232C,
    DateInvalid = 0x232F,
    GotoNoLabel = 0x2330,
    NotRecognized = 0x2331,
    TimeInvalid = 0x2332,
    FilesCopied = 0x2336,
    CurrentDate = 0x2337,
    CurrentTime = 0x2338,
    DirectoryOf = 0x2339,
    EnterTime = 0x2343,
    EchoOff = 0x2345,
    EchoOn = 0x2346,
    VerifyOff = 0x2347,
    VerifyOn = 0x2348,
    CopyOntoItself = 0x2349,
    Unexpected = 0x234A,
    DuplicateOrNotFound = 0x234D,
    AreYouSure = 0x234E,
    More = 0x2352,
    VolumeSerial = 0x235B,
    AlreadyExists = 0x235C,
    VolumeNoLabel = 0x235E,
    VolumeLabel = 0x235F,
    StartBadParameter = 0x2362,
    CannotFindFile = 0x2363,
    BatchNotFound = 0x236C,
    DirsMoved = 0x236D,
    FilesMoved = 0x236E,
    InvalidSwitch = 0x2375,
    FilesSummary = 0x2378,
    DirsSummary = 0x2379,
    TotalFilesListed = 0x237A,
    TerminateBatch = 0x237B,
    DeletedFile = 0x400023A1,
    AssocNotFound = 0x400023A3,
    FtypeNotFound = 0x400023A5,
    SetlocalBadParameter = 0x400023A6,
    VariableNotDefined = 0x400023A9,
    CallLabelOutsideBatch = 0x400023AA,
    LabelNotFound = 0x400023AB,
    UnbalancedParenthesis = 0x400023CC,
    MissingOperand = 0x400023CD,
    MissingOperator = 0x400023CE,
    InvalidNumber = 0x400023CF,
    DivideByZero = 0x400023D1,
    OverwritePrompt = 0x400023D3,
    NoYesAllKeys = 0x400023D4,
    FileNotFound = 0x40002711,
    CouldNotFind = 0x40002712,
    EnterDate = 0x4000271E,
    VerifyError = 0x4000271F,
    MoveManyToOne = 0x40002720,
    HardlinkCreated = 0x4000272B,
    SymlinkCreated = 0x4000272C,
    JunctionCreated = 0x4000272D,
};

inline const char* englishMessage(Msg id) {
    switch (id) {
    case Msg::NoYesKeys: return "NY";
    case Msg::IncorrectParameter: return "An incorrect parameter was\r\nentered for the command.";
    case Msg::SyntaxIncorrect: return "The syntax of the command is incorrect.";
    case Msg::PressAnyKey: return "Press any key to continue . . . ";
    case Msg::DeletePrompt: return "%1, Delete (Y/N)? ";
    case Msg::DateInvalid: return "The system cannot accept the date entered.";
    case Msg::GotoNoLabel: return "No batch label specified to GOTO command.";
    case Msg::NotRecognized: return "'%1' is not recognized as an internal or external command,\r\noperable program or batch file.";
    case Msg::TimeInvalid: return "The system cannot accept the time entered.";
    case Msg::FilesCopied: return "%1 file(s) copied.";
    case Msg::CurrentDate: return "The current date is: ";
    case Msg::CurrentTime: return "The current time is: ";
    case Msg::DirectoryOf: return " Directory of %1\r\n";
    case Msg::EnterTime: return "Enter the new time: ";
    case Msg::EchoOff: return "ECHO is off.";
    case Msg::EchoOn: return "ECHO is on.";
    case Msg::VerifyOff: return "VERIFY is off.";
    case Msg::VerifyOn: return "VERIFY is on.";
    case Msg::CopyOntoItself: return "The file cannot be copied onto itself.";
    case Msg::Unexpected: return "%1 was unexpected at this time.";
    case Msg::DuplicateOrNotFound: return "A duplicate file name exists, or the file\r\ncannot be found.";
    case Msg::AreYouSure: return "%1, Are you sure (Y/N)? ";
    case Msg::More: return "More? ";
    case Msg::VolumeSerial: return " Volume Serial Number is %1";
    case Msg::AlreadyExists: return "A subdirectory or file %1 already exists.";
    case Msg::VolumeNoLabel: return " Volume in drive %1 has no label.";
    case Msg::VolumeLabel: return " Volume in drive %1 is %2";
    case Msg::StartBadParameter: return "The system cannot accept the START command parameter %1.";
    case Msg::CannotFindFile: return "The system cannot find the file %1.";
    case Msg::BatchNotFound: return "The batch file cannot be found.";
    case Msg::DirsMoved: return "%1 dir(s) moved.";
    case Msg::FilesMoved: return "%1 file(s) moved.";
    case Msg::InvalidSwitch: return "Invalid switch - \"%1\".";
    case Msg::FilesSummary: return "     %1 File(s) %2 bytes";
    case Msg::DirsSummary: return "     %1 Dir(s)  %2 bytes free";
    case Msg::TotalFilesListed: return "     Total Files Listed:";
    case Msg::TerminateBatch: return "Terminate batch job (Y/N)? ";
    case Msg::DeletedFile: return "Deleted file - %1";
    case Msg::AssocNotFound: return "File association not found for extension %1";
    case Msg::FtypeNotFound: return "File type '%1' not found or no open command associated with it.";
    case Msg::SetlocalBadParameter: return "Invalid parameter to SETLOCAL command";
    case Msg::VariableNotDefined: return "Environment variable %1 not defined";
    case Msg::CallLabelOutsideBatch: return "Invalid attempt to call batch label outside of batch script.";
    case Msg::LabelNotFound: return "The system cannot find the batch label specified - %1";
    case Msg::UnbalancedParenthesis: return "Unbalanced parenthesis.";
    case Msg::MissingOperand: return "Missing operand.";
    case Msg::MissingOperator: return "Missing operator.";
    case Msg::InvalidNumber: return "Invalid number.  Numeric constants are either decimal (17),\r\nhexadecimal (0x11), or octal (021).";
    case Msg::DivideByZero: return "Divide by zero error.";
    case Msg::OverwritePrompt: return "Overwrite %1? (Yes/No/All): ";
    case Msg::NoYesAllKeys: return "NYA";
    case Msg::FileNotFound: return "File Not Found";
    case Msg::CouldNotFind: return "Could Not Find %1";
    case Msg::EnterDate: return "Enter the new date: (dd-mm-yy) ";
    case Msg::VerifyError: return "ERROR Verify - %1";
    case Msg::MoveManyToOne: return "Cannot move multiple files to a single file.";
    case Msg::HardlinkCreated: return "Hardlink created for %1 <<===>> %2";
    case Msg::SymlinkCreated: return "symbolic link created for %1 <<===>> %2";
    case Msg::JunctionCreated: return "Junction created for %1 <<===>> %2";
    }
    return "";
}

inline std::string messageText(Msg id, unsigned page) {
    std::string text = os::cmdMessage((unsigned)id);
    if (text.empty()) return englishMessage(id);
    if (text.ends_with("\r\n")) text.resize(text.size() - 2);
    return os::encodeText(text, page);
}

inline std::string insertArguments(const std::string& text, std::initializer_list<std::string_view> args) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        char next = i + 1 < text.size() ? text[i + 1] : 0;
        if (text[i] == '%' && next >= '1' && next <= '9' && (size_t)(next - '1') < args.size()) { out += args.begin()[next - '1']; ++i; }
        else if (text[i] == '%' && next == '%') { out += '%'; ++i; }
        else out += text[i];
    }
    return out;
}

}
