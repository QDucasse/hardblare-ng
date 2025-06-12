#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "MCTargetDesc/AArch64AddressingModes.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"

using namespace llvm;

#define DEBUG_TYPE "hbng-annotate"

//===----------------------------------------------------------------------===//
// Helper generator functions



//===----------------------------------------------------------------------===//
// Main class definition

enum Size {
  UNUSED = 0,
  BYTE = 8,
  HALF = 16,
  WORD = 32,
  DOUB = 64,
  QUAD = 128
};

namespace {
class AArch64HBNGAnnotate : public MachineFunctionPass {
    const AArch64InstrInfo *TII;
    const TargetRegisterInfo *TRI;
    const MachineRegisterInfo *MRI;

public:
    static char ID;
    AArch64HBNGAnnotate() : MachineFunctionPass(ID) {
      initializeAArch64HBNGAnnotatePass(*PassRegistry::getPassRegistry());
    }

    // Generate annotations for all instructions in a given machine function
    std::vector<std::string> generateAnnotation(MachineInstr &MI);

    // Main entrypoint for LLVM backend passes
    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override {
      return "AArch64 HBNG Annotate";
    }

private:
    void printStrOperand(MachineOperand &MO, raw_string_ostream &OS);
    void printCondition(AArch64CC::CondCode CC, StringRef Op, raw_string_ostream &OS);
    std::vector<std::string> genRdTwoOperandsAnnotation(MachineInstr &MI, StringRef Op, bool Carry, bool Flags);
    std::vector<std::string> genRdImmAnnotation(MachineInstr &MI, StringRef Op, bool Sticky);
    std::vector<std::string> genRdPCAnnotation(MachineInstr &MI, StringRef Op, bool Page);
    std::vector<std::string> genPCOffsetCondAnnotation(MachineInstr &MI, StringRef Op, bool Cond, bool Link);
    std::vector<std::string> genRtAddr(
      MachineInstr &MI, StringRef Op, Size ASize, Size OSize,
      bool IsScaled, bool IsStore, bool IsPair, bool IsIndexed
    );
    std::vector<std::string> genZero(MachineInstr &MI);
};

char AArch64HBNGAnnotate::ID = 0;

} // end anonymous namespace

// Pass initialization
INITIALIZE_PASS(AArch64HBNGAnnotate, DEBUG_TYPE, "HardBlare-NG annotation pass", false, false)


//===----------------------------------------------------------------------===//
// Helper functions

enum NZCVFlags {
  N = 1 << 3,  // Negative
  Z = 1 << 2,  // Zero
  C = 1 << 1,  // Carry
  V = 1 << 0   // Overflow
};

static unsigned getNZCVFlagsUsed(AArch64CC::CondCode CC) {
  switch (CC) {
      case AArch64CC::EQ: return Z;                     // Zero
      case AArch64CC::NE: return Z;                     // Zero
      case AArch64CC::HS: return C;                     // Carry
      case AArch64CC::LO: return C;                     // Carry
      case AArch64CC::MI: return N;                     // Negative
      case AArch64CC::PL: return N;                     // Negative
      case AArch64CC::VS: return V;                     // Overflow
      case AArch64CC::VC: return V;                     // Overflow
      case AArch64CC::HI: return C | Z;                 // Carry & Zero
      case AArch64CC::LS: return C | Z;                 // Carry & Zero
      case AArch64CC::GE: return N | V;                 // Negative & Overflow
      case AArch64CC::LT: return N | V;                 // Negative & Overflow
      case AArch64CC::GT: return N | V | Z;             // Negative, Overflow & Zero
      case AArch64CC::LE: return N | V | Z;             // Negative, Overflow & Zero
      case AArch64CC::AL: case AArch64CC::NV: return 0; // Always / Never (no flags)
      default: return 0; // Unknown condition
  }
}

static void getMemExtendOperator(MachineInstr &MI, Size OSize, AArch64_AM::ShiftExtendType& SET) {
  switch (MI.getOperand(3).getImm()) {
    case 0:
      switch (OSize) {
        case WORD:
          SET = AArch64_AM::UXTW;
          break;
        case DOUB:
          SET = AArch64_AM::UXTX;
          break;
        default:
          llvm_unreachable("Should be one of WORD/DOUBLE");
          break;
      }
      break;
    case 1:
      switch (OSize) {
        case WORD:
          SET = AArch64_AM::SXTW;
          break;
        case DOUB:
          SET = AArch64_AM::SXTX;
          break;
        default:
          llvm_unreachable("Should be one of WORD/DOUBLE");
          break;
      }
      break;
  }
}

static bool containsSVC(MachineInstr &MI) {
  bool IsSVC = false;
  if (MI.isInlineAsm()) {
    StringRef AsmStr = MI.getOperand(0).getSymbolName();
    if (AsmStr.contains("svc")) {
      IsSVC = true;
    }
  }
  return IsSVC;
}

void AArch64HBNGAnnotate::printCondition(AArch64CC::CondCode CC, StringRef Op, raw_string_ostream &OS) {
  unsigned CCBitMask = getNZCVFlagsUsed(CC);
  if (CCBitMask & N) OS << " " << Op << " N";
  if (CCBitMask & Z) OS << " " << Op << " Z";
  if (CCBitMask & C) OS << " " << Op << " C";
  if (CCBitMask & V) OS << " " << Op << " V";
}

void AArch64HBNGAnnotate::printStrOperand(MachineOperand &MO, raw_string_ostream &OS) {
  if (MO.isReg()) {
    unsigned Reg = MO.getReg();
    OS << TRI->getRegAsmName(Reg);
  } else if (MO.isImm()) {
    OS << "Imm(" << MO.getImm() << ")";
  } else {
    OS << "UOp";
  }
}

//===----------------------------------------------------------------------===//
// Specific annotation functions

std::vector<std::string> AArch64HBNGAnnotate::genZero(MachineInstr &MI) {
  std::string Result;
  raw_string_ostream OS(Result);
  printStrOperand(MI.getOperand(0), OS);
  // FIXME: Passing imm(0) might not be enough to mean REPLACE the previous tag with 0
  OS << " <- Imm(0)";
  return {Result};
}

std::vector<std::string> AArch64HBNGAnnotate::genRdTwoOperandsAnnotation(MachineInstr &MI, StringRef Op, bool Carry, bool Flags) {
  // Annotations vector
  std::vector<std::string> Annotations = {};
  // Use a raw_string_ostream to format the string.
  std::string Result;
  raw_string_ostream OS(Result);

  printStrOperand(MI.getOperand(0), OS);
  OS << " <- ";
  printStrOperand(MI.getOperand(1), OS);
  OS << " " << Op << " ";
  printStrOperand(MI.getOperand(2), OS);

  // Check if the tag of the Carry or Flags needs to be propagated
  if (Carry) {
    OS << " " << Op << " C";
  }
  // Make a reference to the string and store it in the annotation
  LLVM_DEBUG(dbgs() << Result << "\n");
  Annotations.push_back(Result + "\n");

  if (Flags) {
    std::string Result2;
    raw_string_ostream OS2(Result2);
    OS2 << "NZCV <- ";
    printStrOperand(MI.getOperand(1), OS2);
    OS2 << " " << Op << " ";
    printStrOperand(MI.getOperand(2), OS2);
    if (Carry) {
      OS2 << " " << Op << " C";
    }
    LLVM_DEBUG(dbgs() << Result2 << "\n");
    Annotations.push_back(Result2 + "\n");
  }

  return Annotations;
}

std::vector<std::string> AArch64HBNGAnnotate::genRdImmAnnotation(MachineInstr &MI, StringRef Op, bool Sticky) {
  // Use a raw_string_ostream to format the string.
  std::string Result;
  raw_string_ostream OS(Result);

  printStrOperand(MI.getOperand(0), OS);
  OS << " <- ";
  if (Sticky) {
    printStrOperand(MI.getOperand(0), OS);
    OS << " " << Op << " ";
  }
  printStrOperand(MI.getOperand(1), OS);

  LLVM_DEBUG(dbgs() << Result << "\n");
  return {Result + "\n"};
}

std::vector<std::string> AArch64HBNGAnnotate::genRdPCAnnotation(MachineInstr &MI, StringRef Op, bool Page) {
  // Use a raw_string_ostream to format the string.
  std::string Result;
  raw_string_ostream OS(Result);

  printStrOperand(MI.getOperand(0), OS);
  OS << " <- PC";

  // Propagating the code memory tag
#ifdef false
  OS << " " << "[<PC> + ";
  printStrOperand(MI.getOperand(1), OS)
  if (Page) {
    OS << " << 12"
  }
  OS << "]"
#endif

  LLVM_DEBUG(dbgs() << Result << "\n");
  return {Result};
}

std::vector<std::string> AArch64HBNGAnnotate::genPCOffsetCondAnnotation(MachineInstr &MI, StringRef Op, bool Cond, bool Link) {
  std::vector<std::string> Annotations = {};

  int OffsetIndex = Cond ? 1 : 0;

  if (Link) {
    std::string ResultLR;
    raw_string_ostream OSLR(ResultLR);
    OSLR << "LR <- PC " << Op << " LR " << Op << " ";
    printStrOperand(MI.getOperand(OffsetIndex), OSLR);
    LLVM_DEBUG(dbgs() << ResultLR << "\n");
    Annotations.push_back(ResultLR + "\n");
  }

  // Use a raw_string_ostream to format the string.
  std::string ResultPC;
  raw_string_ostream OSPC(ResultPC);

  // PC<- imm + PC + [<PC> + imm]
  OSPC << "PC <- PC " << Op << " ";
  printStrOperand(MI.getOperand(OffsetIndex), OSPC);
  if (Cond) {
    AArch64CC::CondCode CC = static_cast<AArch64CC::CondCode>(MI.getOperand(0).getImm());
    printCondition(CC, Op, OSPC);
  }

#ifdef false
  // TODO: Note that the ImmIndex now is not defined yet and refers to another basic block
  // While it might not be needed (tagging code memory is useful for self-modifying code)
  // it should be happening after the branch resolution (in the MCInst layer) if needed
  OSPC << "[<PC> + ";
  printStrOperand(MI.getOperand(ImmIndex), OSPC);
  OSPC << "] " << Op << " ";
#endif

  LLVM_DEBUG(dbgs() << ResultPC << "\n");
  Annotations.push_back(ResultPC + "\n");
  return Annotations;
}

std::vector<std::string> AArch64HBNGAnnotate::genRtAddr(
  MachineInstr &MI, StringRef Op, Size ASize, Size OSize,
  bool IsScaled, bool IsStore, bool IsPair, bool IsIndexed
) {

  // TODO: This does not output the correct extension operator for an unknown reason.
  // From AArch64SchedPredicates.td:
  // // Check the extension type in the register offset addressing mode.
  // let FunctionMapper = "AArch64_AM::getMemExtendType" in {
  //    def CheckMemExtUXTW                   : CheckImmOperand_s<3, "AArch64_AM::UXTW">;
  //    def CheckMemExtLSL                    : CheckImmOperand_s<3, "AArch64_AM::UXTX">;
  //    def CheckMemExtSXTW                   : CheckImmOperand_s<3, "AArch64_AM::SXTW">;
  //    def CheckMemExtSXTX                   : CheckImmOperand_s<3, "AArch64_AM::SXTX">;
  // }
  // for the "do shift":
  // // Check for scaling in the register offset addressing mode.
  // let FunctionMapper = "AArch64_AM::getMemDoShift" in
  // def CheckMemScaled                      : CheckImmOperandSimple<4>;
  //
  // Note that the CheckImmOperandSimple looks at field 4, same for CheckImmOperand_s looking at field 3.
  //
  // To explore:
  // uint64_t TSFlags = MI.getDesc().TSFlags;
  //
  // FIX (is it correct though?)
  // As the 3rd operand is either 0 or 1, and the four possible extension mode are:
  // UXTW, UXTX, SXTW, SXTX
  // I guess the extension is either signed (1) or unsigned (0) and its size corresponds to the size of the offset register

  std::vector<std::string> Annotations = {};

  int OffsetIndex = IsPair ? 3 : 2;
  if (IsIndexed) {
    OffsetIndex++;
  }

  // Use a raw_string_ostream to format the string.
  std::string ResultAddress;
  raw_string_ostream OSaddr(ResultAddress);

  OSaddr << "[<";
  printStrOperand(MI.getOperand(OffsetIndex - 1), OSaddr);
  OSaddr << "> + ";
  // Scale factor depends on the instruction access size: https://devblogs.microsoft.com/oldnewthing/20220728-00/?p=106912
  if (IsScaled) {
    OSaddr << ASize/8 << "*";
  }
  if (MI.getOperand(OffsetIndex).isReg()) {
    OSaddr << "<";
    printStrOperand(MI.getOperand(OffsetIndex), OSaddr);
    OSaddr << "> ";

    // Get the extension operator from the third operand (0 - unsigned, 1 - signed)
    AArch64_AM::ShiftExtendType SET;
    getMemExtendOperator(MI, OSize, SET);

    bool DoShift = AArch64_AM::getMemDoShift(MI.getOperand(4).getImm());
    OSaddr << AArch64_AM::getShiftExtendName(SET)<< " ";;
    if (DoShift) {
      OSaddr << (ASize >> 4);
    } else {
      OSaddr << "0";
    }
  } else {
    printStrOperand(MI.getOperand(OffsetIndex), OSaddr);
  }
  // Does not close the address ] so that in case of a pair the offset is kept

  // Output the annotation in the form:
  // Rt <- [<Rn> + 4*(<Rm> extend amount)] Op Rn Op Rm
  for (int i = IsPair ? 1 : 0; i < OffsetIndex - 1; i ++) {

    std::string Result;
    raw_string_ostream OS(Result);
    if (IsStore) {
      OS << ResultAddress << "]_" << ASize << " <- ";
      printStrOperand(MI.getOperand(i), OS);
    } else {
      printStrOperand(MI.getOperand(i), OS);
      OS << " <- " << ResultAddress << "]_" << ASize;
    }
    OS << " " << Op << " ";
    printStrOperand(MI.getOperand(OffsetIndex - 1), OS);
    OS << " " << Op << " ";
    printStrOperand(MI.getOperand(OffsetIndex), OS);

    // Mitigate size for a pair if needed
    OSaddr << " + " << ASize;

    LLVM_DEBUG(dbgs() << Result << "\n");
    Annotations.push_back(Result + "\n");
  }

  if (IsIndexed) {
    std::string ResultIndex;
    raw_string_ostream OSIdx(ResultIndex);
    printStrOperand(MI.getOperand(OffsetIndex - 1), OSIdx);
    OSIdx << " <- ";
    printStrOperand(MI.getOperand(OffsetIndex - 1), OSIdx);
    OSIdx << " " << Op << " ";
    printStrOperand(MI.getOperand(OffsetIndex), OSIdx);

    LLVM_DEBUG(dbgs() << ResultIndex << "\n");
    Annotations.push_back(ResultIndex + "\n");
  }

  return Annotations;
}

//===----------------------------------------------------------------------===//
// Main annotation generation function

std::vector<std::string> AArch64HBNGAnnotate::generateAnnotation(MachineInstr &MI) {
  std::vector<std::string> Annotations = {};

  std::string Unsupported;
  raw_string_ostream OS(Unsupported);
  OS << "Unsupported: " << TII->getName(MI.getOpcode());

  if (TII->isGPRZero(MI)) {
    return genZero(MI);
  }

  switch (MI.getOpcode()) {
    default:
        Annotations.push_back(Unsupported + "\n");
        LLVM_DEBUG(dbgs() << Unsupported << "\n");
        break;
    //=== ARITHMETIC ===//
    // Two registers
    case AArch64::ADDXrr: case AArch64::ADDXrx: case AArch64::ADDXrs:
    case AArch64::ADDWrr: case AArch64::ADDWrx: case AArch64::ADDWrs:
      Annotations = genRdTwoOperandsAnnotation(MI, "add", /*Carry=*/false, /*Flags=*/false);
      break;
    case AArch64::SUBXrr: case AArch64::SUBXrx: case AArch64::SUBXrs:
    case AArch64::SUBWrr: case AArch64::SUBWrx: case AArch64::SUBWrs:
      Annotations = genRdTwoOperandsAnnotation(MI, "sub", /*Carry=*/false, /*Flags=*/false);
      break;
    case AArch64::ADDSXrr: case AArch64::ADDSXrx: case AArch64::ADDSXrs:
    case AArch64::ADDSWrr: case AArch64::ADDSWrx: case AArch64::ADDSWrs:
      Annotations = genRdTwoOperandsAnnotation(MI, "add", /*Carry=*/false, /*Flags=*/true);
      break;
    case AArch64::SUBSXrr: case AArch64::SUBSXrx: case AArch64::SUBSXrs:
    case AArch64::SUBSWrr: case AArch64::SUBSWrx: case AArch64::SUBSWrs:
      Annotations = genRdTwoOperandsAnnotation(MI, "sub", /*Carry=*/false, /*Flags=*/true);
      break;
    case AArch64::ADCSXr: case AArch64::ADCSWr:
    case AArch64::SBCSXr: case AArch64::SBCSWr:
      Annotations = genRdTwoOperandsAnnotation(MI, "ari", /*Carry=*/true, /*Flags=*/true);
      break;
    case AArch64::ADCXr: case AArch64::ADCWr:
    case AArch64::SBCXr: case AArch64::SBCWr:
      Annotations = genRdTwoOperandsAnnotation(MI, "ari", /*Carry=*/true, /*Flags=*/false);
      break;
    // Register/Immediate
    case AArch64::ADDXri: case AArch64::ADDWri:
      Annotations = genRdTwoOperandsAnnotation(MI, "add", /*Carry=*/true, /*Flags=*/true);
      break;
    case AArch64::SUBXri: case AArch64::SUBWri:
      Annotations = genRdTwoOperandsAnnotation(MI, "sub", /*Carry=*/false, /*Flags=*/false);
      break;
    case AArch64::ADDSXri: case AArch64::ADDSWri:
    case AArch64::SUBSXri: case AArch64::SUBSWri:
      Annotations = genRdTwoOperandsAnnotation(MI, "ari", /*Carry=*/false, /*Flags=*/true);
      break;

    // TODO: Missing 3 register instructions

    //=== ADDR COMPUTATION ===//
    case AArch64::ADR:
      Annotations = genRdPCAnnotation(MI, "adr", /*Page=*/false);
      break;
    case AArch64::ADRP:
      Annotations = genRdPCAnnotation(MI, "adr", /*Page=*/true);
      break;
    //=== LOGICAL ===//
    case AArch64::ANDXrr: case AArch64::ANDXrs: case AArch64::ANDWrr: case AArch64::ANDWrs:
    case AArch64::BICXrr: case AArch64::BICXrs: case AArch64::BICWrr: case AArch64::BICWrs:
    case AArch64::EORXrr: case AArch64::EORXrs: case AArch64::EORWrr: case AArch64::EORWrs:
    case AArch64::EONXrr: case AArch64::EONXrs: case AArch64::EONWrr: case AArch64::EONWrs:
    case AArch64::ORNXrr: case AArch64::ORNXrs: case AArch64::ORNWrr: case AArch64::ORNWrs:
    case AArch64::ORRXrr: case AArch64::ORRXrs: case AArch64::ORRWrr: case AArch64::ORRWrs:
    // imm
    case AArch64::ORRXri: case AArch64::ORRWri: case AArch64::ANDXri: case AArch64::ANDWri:
      Annotations = genRdTwoOperandsAnnotation(MI, "log", /*Carry=*/false, /*Flags=*/false);
      break;
    case AArch64::ANDSXrr: case AArch64::ANDSXrs: case AArch64::ANDSWrr: case AArch64::ANDSWrs:
    case AArch64::BICSXrr: case AArch64::BICSXrs: case AArch64::BICSWrr: case AArch64::BICSWrs:
    case AArch64::ANDSXri: case AArch64::ANDSWri:
      Annotations = genRdTwoOperandsAnnotation(MI, "log", /*Carry=*/false, /*Flags=*/true);
      break;
    //=== SHIFTS ===//
    case AArch64::ASRVWr: case AArch64::ASRVXr:
    case AArch64::LSRVWr: case AArch64::LSRVXr:
      Annotations = genRdTwoOperandsAnnotation(MI, "shi", /*Carry=*/false, /*Flags=*/true);
      break;
    //=== MOVES ===//
    // Partial move
    case AArch64::MOVKWi: case AArch64::MOVKXi:
      Annotations = genRdImmAnnotation(MI, "mov", /*Sticky=*/true);
      break;
    // Total move
    case AArch64::MOVNWi: case AArch64::MOVNXi:
    case AArch64::MOVZXi: case AArch64::MOVZWi:
      Annotations = genRdImmAnnotation(MI, "mov", /*Sticky=*/false);
      break;
    //=== BITMANIP ===//
    //=== BRANCH ===//
    case AArch64::B:
      Annotations = genPCOffsetCondAnnotation(MI, "bra", /*Cond=*/false, /*Link=*/false);
      break;
    case AArch64::BR:
      Annotations = genPCOffsetCondAnnotation(MI, "bra", /*Cond=*/false, /*Link=*/false);
      break;
    case AArch64::Bcc:
      Annotations = genPCOffsetCondAnnotation(MI, "bra", /*Cond=*/true, /*Link=*/false);
      break;
    //=== BRANCH&LINK ===//
    case AArch64::BL:
      Annotations = genPCOffsetCondAnnotation(MI, "lin", /*Cond=*/false, /*Link=*/true);
      break;
    case AArch64::BLR:
      Annotations = genPCOffsetCondAnnotation(MI, "lin", /*Cond=*/false, /*Link=*/true);
      break;
    case AArch64::RET:
      Annotations = genPCOffsetCondAnnotation(MI, "ret", /*Cond=*/false, /*Link=*/false);
      break;
    //=== LOAD ===//
    // TODO: Other addressing modes
    // Register offsets
    case AArch64::LDRBBroW: case AArch64::LDRBroW:
      Annotations = genRtAddr(MI, "loa", BYTE, WORD, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRBBroX: case AArch64::LDRBroX:
      Annotations = genRtAddr(MI, "loa", BYTE, DOUB, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRHHroW: case AArch64::LDRHroW:
      Annotations = genRtAddr(MI, "loa", HALF, WORD, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
      case AArch64::LDRHHroX: case AArch64::LDRHroX:
      Annotations = genRtAddr(MI, "loa", HALF, DOUB, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRWroW:  case AArch64::LDRSWroW: case AArch64::LDRSroW:
      Annotations = genRtAddr(MI, "loa", WORD, WORD, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRWroX: case AArch64::LDRSWroX: case AArch64::LDRSroX:
      Annotations = genRtAddr(MI, "loa", WORD, DOUB, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRXroW: case AArch64::LDRDroW:
      Annotations = genRtAddr(MI, "sto", DOUB, WORD, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRXroX: case AArch64::LDRDroX:
      Annotations = genRtAddr(MI, "loa", DOUB, DOUB, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    // Immediate offsets
    case AArch64::LDRBBui: case AArch64::LDRBui:
      Annotations = genRtAddr(MI, "loa", BYTE, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRHHui: case AArch64::LDRHui:
      Annotations = genRtAddr(MI, "loa", HALF, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRWui: case AArch64::LDRSWui: case AArch64::LDRSui:
      Annotations = genRtAddr(MI, "loa", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRXui: case AArch64::LDRDui:
      Annotations = genRtAddr(MI, "loa", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRQui:
      Annotations = genRtAddr(MI, "loa", QUAD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    // TODO: Difference between ldurswi and ldursi?
    case AArch64::LDURWi: case AArch64::LDURSWi: case AArch64::LDURSi:
      Annotations = genRtAddr(MI, "loa", WORD, UNUSED, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDURXi: case AArch64::LDURDi:
      Annotations = genRtAddr(MI, "loa", DOUB, UNUSED, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRBBpre: case AArch64::LDRBBpost: case AArch64::LDRBpre: case AArch64::LDRBpost:
      Annotations = genRtAddr(MI, "loa", BYTE, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::LDRHHpre: case AArch64::LDRHHpost: case AArch64::LDRHpre: case AArch64::LDRHpost:
      Annotations = genRtAddr(MI, "loa", HALF, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::LDRWpre: case AArch64::LDRWpost: case AArch64::LDRSpre: case AArch64::LDRSpost:
      Annotations = genRtAddr(MI, "loa", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::LDRXpre: case AArch64::LDRXpost: case AArch64::LDRDpre: case AArch64::LDRDpost:
      Annotations = genRtAddr(MI, "loa", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/true);
      break;
    // Pairs
    case AArch64::LDPWi: case AArch64::LDPSWi: case AArch64::LDPSi:
      Annotations = genRtAddr(MI, "loa", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/true, /*isIndexed*/false);
      break;
    case AArch64::LDPXi: case AArch64::LDPDi:
      Annotations = genRtAddr(MI, "loa", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/true, /*isIndexed*/false);
      break;
    case AArch64::LDPQi:
      Annotations = genRtAddr(MI, "loa", QUAD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/true, /*isIndexed*/false);
      break;
    case AArch64::LDPWpre: case AArch64::LDPWpost: case AArch64::LDPDpost:
      Annotations = genRtAddr(MI, "loa", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/true, /*isIndexed*/true);
      break;
    case AArch64::LDPXpre: case AArch64::LDPXpost:
      Annotations = genRtAddr(MI, "loa", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/true, /*isIndexed*/true);
      break;
    //=== STORE ===//
    // TODO: Other addressing modes
    // Register offsets
    case AArch64::STRBBroW: case AArch64::STRBroW:
      Annotations = genRtAddr(MI, "sto", BYTE, WORD, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRBBroX: case AArch64::STRBroX:
      Annotations = genRtAddr(MI, "sto", BYTE, DOUB, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRHHroW: case AArch64::STRHroW:
      Annotations = genRtAddr(MI, "sto", HALF, WORD, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRHHroX: case AArch64::STRHroX:
      Annotations = genRtAddr(MI, "sto", HALF, DOUB, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRWroW: case AArch64::STRSroW:
      Annotations = genRtAddr(MI, "sto", WORD, WORD, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRWroX: case AArch64::STRSroX:
      Annotations = genRtAddr(MI, "sto", WORD, DOUB, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRXroW: case AArch64::STRDroW:
      Annotations = genRtAddr(MI, "sto", DOUB, WORD, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRXroX: case AArch64::STRDroX:
      Annotations = genRtAddr(MI, "sto", DOUB, DOUB, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    // Immediate offsets
    case AArch64::STRWui: case AArch64::STRSui:
      Annotations = genRtAddr(MI, "sto", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRXui: case AArch64::STRDui:
      Annotations = genRtAddr(MI, "sto", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRQui:
      Annotations = genRtAddr(MI, "sto", QUAD, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STURWi:
      Annotations = genRtAddr(MI, "sto", WORD, UNUSED, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STURXi: case AArch64::STURDi:
      Annotations = genRtAddr(MI, "sto", DOUB, UNUSED, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRBBpre: case AArch64::STRBBpost: case AArch64::STRBpre: case AArch64::STRBpost:
      Annotations = genRtAddr(MI, "sto", BYTE, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::STRHHpre: case AArch64::STRHHpost: case AArch64::STRHpre: case AArch64::STRHpost:
      Annotations = genRtAddr(MI, "sto", HALF, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::STRWpost: case AArch64::STRWpre:
      Annotations = genRtAddr(MI, "sto", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::STRXpost: case AArch64::STRXpre:
      Annotations = genRtAddr(MI, "sto", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/true);
      break;
    // Pairs
    case AArch64::STPWi: case AArch64::STPSi:
      Annotations = genRtAddr(MI, "sto", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/true, /*isIndexed*/false);
      break;
    case AArch64::STPXi: case AArch64::STPDi:
      Annotations = genRtAddr(MI, "sto", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/true, /*isIndexed*/false);
      break;
    case AArch64::STPXpre: case AArch64::STPXpost: case AArch64::STPDpre: case AArch64::STPDpost:
      Annotations = genRtAddr(MI, "sto", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/true, /*isIndexed*/true);
      break;
    // UNSUPPORTED
    case AArch64::CFI_INSTRUCTION:
    case AArch64::DBG_VALUE:
    case AArch64::DBG_VALUE_LIST:
    case AArch64::IMPLICIT_DEF:
    case AArch64::JUMP_TABLE_DEBUG_INFO:
    case AArch64::JumpTableDest16:
    case AArch64::KILL:
      break;
  }
  return Annotations;
}

//===----------------------------------------------------------------------===//
// Main loop

bool AArch64HBNGAnnotate::runOnMachineFunction(MachineFunction &MF) {
    LLVM_DEBUG(dbgs() << "***** HBNG Annotations *****\n");

    // TODO: Would be nice but activating it breaks the generation of temporary symbols
    // const Function &F = MF.getFunction();
    // if (F.hasFnAttribute("hbng_no_instr")) {
    //   // FIXME: temporary labels are not emitted I dont know why
    //   for (auto &MBB : MF) MBB.setLabelMustBeEmitted();
    //   LLVM_DEBUG(dbgs() << "Skipping instrumentation for " << F.getName() << "\n");
    //   return false;
    // }


    // Initialize subtarget information as attributes
    auto &STI = MF.getSubtarget<AArch64Subtarget>();
    TII = STI.getInstrInfo();
    TRI = MF.getSubtarget().getRegisterInfo();
    MRI = &MF.getRegInfo();
    // Function Info, the basic block table is stored here
    AArch64FunctionInfo *FI = MF.getInfo<AArch64FunctionInfo>();

    LLVM_DEBUG(dbgs() << "func: " << MF.getName() << "\n");
    for (auto &MBB : MF) {
      LLVM_DEBUG(dbgs() << "bblock: " << MBB.getName() << "\n");
      // Get the symbol of the basic block
      Twine SymbolName = MBB.isEntryBlock()
        ? "HBNG_" + Twine(MF.getFunction().getName())
        : "HBNG_" + Twine(MF.getFunction().getName()) + "_" + Twine(MBB.getNumber());
      MCSymbol *BBSym = MF.getContext().getOrCreateSymbol(SymbolName);
      MCSymbol *BBAnnotSym = MF.getContext().getOrCreateSymbol(SymbolName + "_annot");

      LLVM_DEBUG(dbgs() << "ri block name: " << BBSym->getName() << "\n");

      // Adds the basic block symbol and start offset of the annotations in the table
      HBNGAnnotationInfo BBAnnotationInfo;
      BBAnnotationInfo.BBSymbol = BBSym;
      BBAnnotationInfo.BBAnnotSymbol = BBAnnotSym;
      BBAnnotationInfo.Annotations = {};


      unsigned SplitIndex = 0;

      for (auto MIIt = MBB.begin(), End = MBB.end(); MIIt != End; ++MIIt) {
        MachineInstr &MI = *MIIt;
        unsigned int Opcode = MI.getOpcode();
        StringRef Mnemonic = TII->getName(Opcode);
        LLVM_DEBUG(dbgs() << "instr: " << Mnemonic << "\n");
        // Main generation function, creates AND STORES the annotation
        std::vector<std::string> InstrAnnotations = generateAnnotation(MI);
        BBAnnotationInfo.Annotations.insert(
          BBAnnotationInfo.Annotations.end(),
          InstrAnnotations.begin(),
          InstrAnnotations.end()
        );

        // Check if the current instruction is a branch/return/call etc. but the
        // basic block does not end on it. If this is the case, we have to emit
        // a symbol and push it to the bbt to comply to the CoreSight way of defining
        // Atoms.
        bool IsLastInstr = std::next(MIIt) == End;
        bool IsCtrlFlow = MI.isBranch() || MI.isReturn() || MI.isCall();
        if ((IsCtrlFlow || containsSVC(MI)) && !IsLastInstr) {
          BBAnnotationInfo.Annotations.push_back("END\n");
          FI->BBAnnotationInfos.push_back(BBAnnotationInfo);

          // Insert the pseudo instruction in the MBB
          BuildMI(MBB, std::next(MIIt), DebugLoc(), TII->get(AArch64::HBNG_ATOM_LABEL));

          // Create the new symbols, splitting the basic block and annotations
          Twine SplitBaseName = SymbolName + "_split_" + Twine(SplitIndex++);
          MCSymbol *SplitBBSym = MF.getContext().getOrCreateSymbol(SplitBaseName);
          MCSymbol *SplitAnnotSym = MF.getContext().getOrCreateSymbol(SplitBaseName + "_annot");
          // Reset the BBAnnotation
          BBAnnotationInfo.BBSymbol = SplitBBSym;
          BBAnnotationInfo.BBAnnotSymbol = SplitAnnotSym;
          BBAnnotationInfo.Annotations = {};

          LLVM_DEBUG(dbgs() << "Splitting the block, resetting info with: " << SplitBBSym->getName() << " and " << SplitAnnotSym->getName() << "\n");
        }
      }

      // Emit END annotation only if block ends in a branch
      if (!MBB.empty()) {
        MachineInstr &LastMI = MBB.back();
        if (LastMI.isBranch() || LastMI.isReturn() || LastMI.isCall() || containsSVC(LastMI)) {
          std::string EndAnnotation = "END\n";
          BBAnnotationInfo.Annotations.push_back(EndAnnotation);
        }
      }

      FI->BBAnnotationInfos.push_back(BBAnnotationInfo);
    }

    // Notify if the content has changed
    return false;
}


// Factory function used by AArch64TargetMachine to add the pass to
// the passmanager.
FunctionPass *llvm::createAArch64HBNGAnnotatePass() {
  return new AArch64HBNGAnnotate();
}