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
    std::vector<MDNode *> Annotations;
    uint64_t CurrentAnnotationOffset;
    std::vector<std::pair<std::string, uint64_t>> BasicBlockTable;

public:
    static char ID;
    AArch64HBNGAnnotate() : MachineFunctionPass(ID) {
      initializeAArch64HBNGAnnotatePass(*PassRegistry::getPassRegistry());
      CurrentAnnotationOffset = 0;
    }

    // Generate annotations for all instructions in a given machine function
    void generateAnnotation(MachineInstr &MI, LLVMContext &Ctx);

    // Main entrypoint for LLVM backend passes
    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override {
      return "AArch64 HBNG Annotate";
    }

private:
    void storeAnnotation(StringRef &Annotation, LLVMContext &Ctx);
    void printStrOperand(MachineOperand &MO, raw_string_ostream &OS);
    void printCondition(AArch64CC::CondCode CC, StringRef Op, raw_string_ostream &OS);
    void genRdTwoOperandsAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Carry, bool Flags);
    void genRdImmAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Sticky);
    void genRdPCAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Page);
    void genPCOffsetCondAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Cond, bool Link);
    void genRtAddr(
      MachineInstr &MI, LLVMContext &Ctx, StringRef Op, Size ASize, Size OSize,
      bool IsScaled, bool IsStore, bool IsPair, bool IsIndexed
    );
    void genZero(MachineInstr &MI, LLVMContext &Ctx);
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

// FIXME: Should probably simply push back and perform this step once at the end of the pass
void AArch64HBNGAnnotate::storeAnnotation(StringRef &Annotation, LLVMContext &Ctx) {
  Metadata *OpData[] = { MDString::get(Ctx, Annotation) };
  Annotations.push_back(MDTuple::get(Ctx, OpData)); // Store metadata in vector
  uint64_t AnnotationSize = Annotation.size();
  CurrentAnnotationOffset += AnnotationSize;
}

//===----------------------------------------------------------------------===//
// Specific annotation functions

void AArch64HBNGAnnotate::genZero(MachineInstr &MI, LLVMContext &Ctx) {
  std::string FormatString;
  raw_string_ostream OS(FormatString);
  printStrOperand(MI.getOperand(0), OS);
  OS << " <- 0";
  StringRef Result2(FormatString);
  LLVM_DEBUG(dbgs() << Result2 << "\n");
  storeAnnotation(Result2, Ctx);
}

void AArch64HBNGAnnotate::genRdTwoOperandsAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Carry, bool Flags) {
  // Use a raw_string_ostream to format the string.
  std::string FormatString;
  raw_string_ostream OS(FormatString);

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
  StringRef Result(FormatString);
  LLVM_DEBUG(dbgs() << Result << "\n");
  storeAnnotation(Result, Ctx);

  if (Flags) {
    std::string FormatString;
    raw_string_ostream OS2(FormatString);
    OS2 << "NZCV <- ";
    printStrOperand(MI.getOperand(1), OS2);
    OS2 << " " << Op << " ";
    printStrOperand(MI.getOperand(2), OS2);
    if (Carry) {
      OS2 << " " << Op << " C";
    }
    StringRef Result2(FormatString);
    LLVM_DEBUG(dbgs() << Result2 << "\n");
    storeAnnotation(Result2, Ctx);
  }

}

void AArch64HBNGAnnotate::genRdImmAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Sticky) {
  // Use a raw_string_ostream to format the string.
  std::string FormatString;
  raw_string_ostream OS(FormatString);

  printStrOperand(MI.getOperand(0), OS);
  OS << " <- ";
  if (Sticky) {
    printStrOperand(MI.getOperand(0), OS);
    OS << " " << Op << " ";
  }
  printStrOperand(MI.getOperand(1), OS);

  StringRef Result(FormatString);
  LLVM_DEBUG(dbgs() << Result << "\n");
  storeAnnotation(Result, Ctx);
}

void AArch64HBNGAnnotate::genRdPCAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Page) {
  // Use a raw_string_ostream to format the string.
  std::string FormatString;
  raw_string_ostream OS(FormatString);

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

  StringRef Result(FormatString);
  LLVM_DEBUG(dbgs() << Result << "\n");
  storeAnnotation(Result, Ctx);
}

void AArch64HBNGAnnotate::genPCOffsetCondAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Cond, bool Link) {

  int OffsetIndex = Cond ? 1 : 0;

  if (Link) {
    std::string FormatStringLR;
    raw_string_ostream OSLR(FormatStringLR);
    OSLR << "LR <- PC " << Op << " LR " << Op << " ";
    printStrOperand(MI.getOperand(OffsetIndex), OSLR);
    StringRef Result(FormatStringLR);
    LLVM_DEBUG(dbgs() << Result << "\n");
    storeAnnotation(Result, Ctx);
  }

  // Use a raw_string_ostream to format the string.
  std::string FormatString;
  raw_string_ostream OSPC(FormatString);

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

  StringRef Result(FormatString);
  LLVM_DEBUG(dbgs() << Result << "\n");
  storeAnnotation(Result, Ctx);
}

void AArch64HBNGAnnotate::genRtAddr(
  MachineInstr &MI, LLVMContext &Ctx, StringRef Op, Size ASize, Size OSize,
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


  int OffsetIndex = IsPair ? 3 : 2;
  if (IsIndexed) {
    OffsetIndex++;
  }

  // Use a raw_string_ostream to format the string.
  std::string FormatStringAddress;
  raw_string_ostream OSaddr(FormatStringAddress);

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

    std::string FormatString;
    raw_string_ostream OS(FormatString);
    if (IsStore) {
      OS << FormatStringAddress << "]_" << ASize << " <- ";
      printStrOperand(MI.getOperand(i), OS);
    } else {
      printStrOperand(MI.getOperand(i), OS);
      OS << " <- " << FormatStringAddress << "]_" << ASize;
    }
    OS << " " << Op << " ";
    printStrOperand(MI.getOperand(OffsetIndex - 1), OS);
    OS << " " << Op << " ";
    printStrOperand(MI.getOperand(OffsetIndex), OS);

    StringRef Result(FormatString);
    LLVM_DEBUG(dbgs() << Result << "\n");
    storeAnnotation(Result, Ctx);

    // Mitigate size for a pair if needed
    OSaddr << " + " << ASize;
  }

  if (IsIndexed) {
    std::string FormatString;
    raw_string_ostream OSIdx(FormatString);
    printStrOperand(MI.getOperand(OffsetIndex - 1), OSIdx);
    OSIdx << " <- ";
    printStrOperand(MI.getOperand(OffsetIndex - 1), OSIdx);
    OSIdx << " " << Op << " ";
    printStrOperand(MI.getOperand(OffsetIndex), OSIdx);
    StringRef Result(FormatString);
    LLVM_DEBUG(dbgs() << Result << "\n");
    storeAnnotation(Result, Ctx);
  }
}

//===----------------------------------------------------------------------===//
// Main annotation generation function

void AArch64HBNGAnnotate::generateAnnotation(MachineInstr &MI, LLVMContext &Ctx) {
  std::string Unsupported;
  raw_string_ostream OS(Unsupported);
  OS << "Unsupported: " << TII->getName(MI.getOpcode());
  StringRef Unsup(Unsupported);

  if (TII->isGPRZero(MI)) {
    genZero(MI, Ctx);
    return;
  }

  switch (MI.getOpcode()) {
    default:
        storeAnnotation(Unsup, Ctx);
        LLVM_DEBUG(dbgs() << Unsupported << "\n");
        break;
    //=== ARITHMETIC ===//
    // Two registers
    case AArch64::ADDXrr: case AArch64::ADDXrx: case AArch64::ADDXrs:
    case AArch64::ADDWrr: case AArch64::ADDWrx: case AArch64::ADDWrs:
    case AArch64::SUBXrr: case AArch64::SUBXrx: case AArch64::SUBXrs:
    case AArch64::SUBWrr: case AArch64::SUBWrx: case AArch64::SUBWrs:
      genRdTwoOperandsAnnotation(MI, Ctx, "ari", /*Carry=*/false, /*Flags=*/false);
      break;
    case AArch64::ADDSXrr: case AArch64::ADDSXrx: case AArch64::ADDSXrs:
    case AArch64::ADDSWrr: case AArch64::ADDSWrx: case AArch64::ADDSWrs:
    case AArch64::SUBSXrr: case AArch64::SUBSXrx: case AArch64::SUBSXrs:
    case AArch64::SUBSWrr: case AArch64::SUBSWrx: case AArch64::SUBSWrs:
      genRdTwoOperandsAnnotation(MI, Ctx, "ari", /*Carry=*/false, /*Flags=*/true);
      break;
    case AArch64::ADCSXr: case AArch64::ADCSWr:
    case AArch64::SBCSXr: case AArch64::SBCSWr:
      genRdTwoOperandsAnnotation(MI, Ctx, "ari", /*Carry=*/true, /*Flags=*/true);
      break;
    case AArch64::ADCXr: case AArch64::ADCWr:
    case AArch64::SBCXr: case AArch64::SBCWr:
      genRdTwoOperandsAnnotation(MI, Ctx, "ari", /*Carry=*/true, /*Flags=*/false);
      break;
    // Register/Immediate
    case AArch64::ADDXri: case AArch64::ADDWri:
    case AArch64::SUBXri: case AArch64::SUBWri:
      genRdTwoOperandsAnnotation(MI, Ctx, "ari", /*Carry=*/false, /*Flags=*/false);
      break;
    case AArch64::ADDSXri: case AArch64::ADDSWri:
    case AArch64::SUBSXri: case AArch64::SUBSWri:
      genRdTwoOperandsAnnotation(MI, Ctx, "ari", /*Carry=*/false, /*Flags=*/true);
      break;

    // TODO: Missing 3 register instructions

    //=== ADDR COMPUTATION ===//
    case AArch64::ADR:
      genRdPCAnnotation(MI, Ctx, "adr", /*Page=*/false);
      break;
    case AArch64::ADRP:
      genRdPCAnnotation(MI, Ctx, "adr", /*Page=*/true);
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
      genRdTwoOperandsAnnotation(MI, Ctx, "log", /*Carry=*/false, /*Flags=*/false);
      break;
    case AArch64::ANDSXrr: case AArch64::ANDSXrs: case AArch64::ANDSWrr: case AArch64::ANDSWrs:
    case AArch64::BICSXrr: case AArch64::BICSXrs: case AArch64::BICSWrr: case AArch64::BICSWrs:
    case AArch64::ANDSXri: case AArch64::ANDSWri:
      genRdTwoOperandsAnnotation(MI, Ctx, "log", /*Carry=*/false, /*Flags=*/true);
      break;
    //=== SHIFTS ===//
    case AArch64::ASRVWr: case AArch64::ASRVXr:
    case AArch64::LSRVWr: case AArch64::LSRVXr:
      genRdTwoOperandsAnnotation(MI, Ctx, "shi", /*Carry=*/false, /*Flags=*/true);
      break;
    //=== MOVES ===//
    // Partial move
    case AArch64::MOVKWi: case AArch64::MOVKXi:
      genRdImmAnnotation(MI, Ctx, "mov", /*Sticky=*/true);
      break;
    // Total move
    case AArch64::MOVNWi: case AArch64::MOVNXi:
    case AArch64::MOVZXi: case AArch64::MOVZWi:
      genRdImmAnnotation(MI, Ctx, "mov", /*Sticky=*/false);
      break;
    //=== BITMANIP ===//
    //=== BRANCH ===//
    case AArch64::B:
      genPCOffsetCondAnnotation(MI, Ctx, "bra", /*Cond=*/false, /*Link=*/false);
      break;
    case AArch64::BR:
      genPCOffsetCondAnnotation(MI, Ctx, "bra", /*Cond=*/false, /*Link=*/false);
      break;
    case AArch64::Bcc:
      genPCOffsetCondAnnotation(MI, Ctx, "bra", /*Cond=*/true, /*Link=*/false);
      break;
    //=== BRANCH&LINK ===//
    case AArch64::BL:
      genPCOffsetCondAnnotation(MI, Ctx, "lin", /*Cond=*/false, /*Link=*/true);
      break;
    case AArch64::BLR:
      genPCOffsetCondAnnotation(MI, Ctx, "lin", /*Cond=*/false, /*Link=*/true);
      break;
    case AArch64::RET:
      genPCOffsetCondAnnotation(MI, Ctx, "ret", /*Cond=*/false, /*Link=*/false);
      break;
    //=== LOAD ===//
    // TODO: Other addressing modes
    // Register offsets
    case AArch64::LDRBBroW: case AArch64::LDRBroW:
      genRtAddr(MI, Ctx, "loa", BYTE, WORD, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRBBroX: case AArch64::LDRBroX:
      genRtAddr(MI, Ctx, "loa", BYTE, DOUB, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRHHroW: case AArch64::LDRHroW:
      genRtAddr(MI, Ctx, "loa", HALF, WORD, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
      case AArch64::LDRHHroX: case AArch64::LDRHroX:
      genRtAddr(MI, Ctx, "loa", HALF, DOUB, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRWroW:  case AArch64::LDRSWroW: case AArch64::LDRSroW:
      genRtAddr(MI, Ctx, "loa", WORD, WORD, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRWroX: case AArch64::LDRSWroX: case AArch64::LDRSroX:
      genRtAddr(MI, Ctx, "loa", WORD, DOUB, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRXroW: case AArch64::LDRDroW:
      genRtAddr(MI, Ctx, "sto", DOUB, WORD, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRXroX: case AArch64::LDRDroX:
      genRtAddr(MI, Ctx, "loa", DOUB, DOUB, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    // Immediate offsets
    case AArch64::LDRBBui: case AArch64::LDRBui:
      genRtAddr(MI, Ctx, "loa", BYTE, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRHHui: case AArch64::LDRHui:
      genRtAddr(MI, Ctx, "loa", HALF, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRWui: case AArch64::LDRSWui: case AArch64::LDRSui:
      genRtAddr(MI, Ctx, "loa", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRXui: case AArch64::LDRDui:
      genRtAddr(MI, Ctx, "loa", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRQui:
      genRtAddr(MI, Ctx, "loa", QUAD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    // TODO: Difference between ldurswi and ldursi?
    case AArch64::LDURWi: case AArch64::LDURSWi: case AArch64::LDURSi:
      genRtAddr(MI, Ctx, "loa", WORD, UNUSED, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDURXi: case AArch64::LDURDi:
      genRtAddr(MI, Ctx, "loa", DOUB, UNUSED, /*isScaled=*/false, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::LDRBBpre: case AArch64::LDRBBpost: case AArch64::LDRBpre: case AArch64::LDRBpost:
      genRtAddr(MI, Ctx, "loa", BYTE, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::LDRHHpre: case AArch64::LDRHHpost: case AArch64::LDRHpre: case AArch64::LDRHpost:
      genRtAddr(MI, Ctx, "loa", HALF, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::LDRWpre: case AArch64::LDRWpost: case AArch64::LDRSpre: case AArch64::LDRSpost:
      genRtAddr(MI, Ctx, "loa", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::LDRXpre: case AArch64::LDRXpost: case AArch64::LDRDpre: case AArch64::LDRDpost:
      genRtAddr(MI, Ctx, "loa", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/false, /*isIndexed*/true);
      break;
    // Pairs
    case AArch64::LDPWi: case AArch64::LDPSWi: case AArch64::LDPSi:
      genRtAddr(MI, Ctx, "loa", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/true, /*isIndexed*/false);
      break;
    case AArch64::LDPXi: case AArch64::LDPDi:
      genRtAddr(MI, Ctx, "loa", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/true, /*isIndexed*/false);
      break;
    case AArch64::LDPQi:
      genRtAddr(MI, Ctx, "loa", QUAD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/true, /*isIndexed*/false);
      break;
    case AArch64::LDPWpre: case AArch64::LDPWpost: case AArch64::LDPDpost:
      genRtAddr(MI, Ctx, "loa", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/true, /*isIndexed*/true);
      break;
    case AArch64::LDPXpre: case AArch64::LDPXpost:
      genRtAddr(MI, Ctx, "loa", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/false, /*isPair=*/true, /*isIndexed*/true);
      break;
    //=== STORE ===//
    // TODO: Other addressing modes
    // Register offsets
    case AArch64::STRBBroW: case AArch64::STRBroW:
      genRtAddr(MI, Ctx, "sto", BYTE, WORD, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRBBroX: case AArch64::STRBroX:
      genRtAddr(MI, Ctx, "sto", BYTE, DOUB, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRHHroW: case AArch64::STRHroW:
      genRtAddr(MI, Ctx, "sto", HALF, WORD, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRHHroX: case AArch64::STRHroX:
      genRtAddr(MI, Ctx, "sto", HALF, DOUB, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRWroW: case AArch64::STRSroW:
      genRtAddr(MI, Ctx, "sto", WORD, WORD, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRWroX: case AArch64::STRSroX:
      genRtAddr(MI, Ctx, "sto", WORD, DOUB, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRXroW: case AArch64::STRDroW:
      genRtAddr(MI, Ctx, "sto", DOUB, WORD, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRXroX: case AArch64::STRDroX:
      genRtAddr(MI, Ctx, "sto", DOUB, DOUB, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    // Immediate offsets
    case AArch64::STRWui: case AArch64::STRSui:
      genRtAddr(MI, Ctx, "sto", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRXui: case AArch64::STRDui:
      genRtAddr(MI, Ctx, "sto", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRQui:
      genRtAddr(MI, Ctx, "sto", QUAD, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STURWi:
      genRtAddr(MI, Ctx, "sto", WORD, UNUSED, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STURXi: case AArch64::STURDi:
      genRtAddr(MI, Ctx, "sto", DOUB, UNUSED, /*isScaled=*/false, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/false);
      break;
    case AArch64::STRBBpre: case AArch64::STRBBpost: case AArch64::STRBpre: case AArch64::STRBpost:
      genRtAddr(MI, Ctx, "sto", BYTE, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::STRHHpre: case AArch64::STRHHpost: case AArch64::STRHpre: case AArch64::STRHpost:
      genRtAddr(MI, Ctx, "sto", HALF, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::STRWpost: case AArch64::STRWpre:
      genRtAddr(MI, Ctx, "sto", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/true);
      break;
    case AArch64::STRXpost: case AArch64::STRXpre:
      genRtAddr(MI, Ctx, "sto", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/false, /*isIndexed*/true);
      break;
    // Pairs
    case AArch64::STPWi: case AArch64::STPSi:
      genRtAddr(MI, Ctx, "sto", WORD, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/true, /*isIndexed*/false);
      break;
    case AArch64::STPXi: case AArch64::STPDi:
      genRtAddr(MI, Ctx, "sto", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/true, /*isIndexed*/false);
      break;
    case AArch64::STPXpre: case AArch64::STPXpost: case AArch64::STPDpre: case AArch64::STPDpost:
      genRtAddr(MI, Ctx, "sto", DOUB, UNUSED, /*isScaled=*/true, /*isStore=*/true, /*isPair=*/true, /*isIndexed*/true);
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
}

//===----------------------------------------------------------------------===//
// Main loop

bool AArch64HBNGAnnotate::runOnMachineFunction(MachineFunction &MF) {
    LLVM_DEBUG(dbgs() << "***** HBNG Annotations *****\n");
    // Initialize subtarget information as attributes
    auto &STI = MF.getSubtarget<AArch64Subtarget>();
    TII = STI.getInstrInfo();
    TRI = MF.getSubtarget().getRegisterInfo();
    MRI = &MF.getRegInfo();
    // Get the context and module
    Module *M = MF.getFunction().getParent();
    LLVMContext &Ctx = MF.getFunction().getContext();

    // Reset the basic block table and annotations for the current machine function
    BasicBlockTable = {};
    Annotations = {};

    // Attach collected data as module metadata, avoids polluting global symbols
    // and will be extracted at the emission stage and added to the binary. We create
    // Two dedicated structures, one for the annotations, the other for the BBT.
    NamedMDNode *AnnotMD = M->getOrInsertNamedMetadata("hbng_annotation_info");
    NamedMDNode *BBTMD = M->getOrInsertNamedMetadata("hbng_basic_block_table");

    LLVM_DEBUG(dbgs() << "func: " << MF.getName() << "\n");
    for (auto &MBB : MF) {
      LLVM_DEBUG(dbgs() << "bblock: " << MBB.getName() << "\n");
      // Basic block symbol
      // TODO: I am here reverse-engineering the temporary label used in the final binary,
      //       This might not be the best option but since the basic block label itself
      //       is not generated yet (MBB.getSymbol() gets an empty string for now).
      //       Note that if the basic block is the first one in the function, its label
      //       will not be generated, even with setLabelMustBeEmitted, (see
      //       shouldEmitLabelForBasicBlock in AsmPrinter.cpp) as it can fall back to the
      //       function name label directly.
      std::string BBSymbolName;
      if (MBB.isEntryBlock()) {
        BBSymbolName = MF.getName();
      } else {
        // TODO: why is the -1 needed..... a basic block might be removed later on?
        BBSymbolName = ".LBB" + std::to_string(MF.getFunctionNumber()) + "_" + std::to_string(MBB.getNumber());
      }
      // Force basic block labels to be emitted
      MBB.setLabelMustBeEmitted();

      // Adds the basic block symbol and start offset of the annotations in the table
      BasicBlockTable.push_back({BBSymbolName, CurrentAnnotationOffset});

      for (auto &MI : MBB) {
        unsigned int Opcode = MI.getOpcode();
        StringRef Mnemonic = TII->getName(Opcode);
        LLVM_DEBUG(dbgs() << "instr: " << Mnemonic << "\n");
        // Main generation function, creates AND STORES the annotation
        generateAnnotation(MI, Ctx);
      }
      // Adding an end instruction in the annotations
      std::string EndAnnotation = "END";
      StringRef EndRef(EndAnnotation);
      storeAnnotation(EndRef, Ctx);
    }

    //
    for (auto &Annotation : Annotations) {
      AnnotMD->addOperand(Annotation);
    }

    for (const auto &Entry : BasicBlockTable) {
        // Create a tuple with the basic block symbol and the corresponding annotation offset
        std::vector<Metadata *> MetadataList;
        MetadataList.push_back(MDString::get(Ctx, Entry.first)); // Basic block symbol (converted to string)
        MetadataList.push_back(ConstantAsMetadata::get(ConstantInt::get(Ctx, APInt(64, Entry.second)))); // Annotation offset (64-bit integer)

        // Add the tuple to the BBT metadata
        BBTMD->addOperand(MDTuple::get(Ctx, MetadataList));
    }

    // Notify if the content has changed
    return false;
}


// Factory function used by AArch64TargetMachine to add the pass to
// the passmanager.
FunctionPass *llvm::createAArch64HBNGAnnotatePass() {
  return new AArch64HBNGAnnotate();
}