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
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "MCTargetDesc/AArch64AddressingModes.h"

using namespace llvm;

#define DEBUG_TYPE "hbng-annotate"

//===----------------------------------------------------------------------===//
// Helper generator functions



//===----------------------------------------------------------------------===//
// Main class definition

enum AccessSize {
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

public:
    static char ID;
    AArch64HBNGAnnotate() : MachineFunctionPass(ID) {
      initializeAArch64HBNGAnnotatePass(*PassRegistry::getPassRegistry());
    }

    // Generate annotations for all instructions in a given machine function
    void generateAnnotation(MachineInstr &MI, LLVMContext &Ctx);

    // Main entrypoint for LLVM backend passes
    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override {
      return "AArch64 HBNG Annotate";
    }

private:
    void storeAnnotation(StringRef &annotation, LLVMContext &Ctx);
    void printStrOperand(MachineOperand &MO, raw_string_ostream &OS);
    void printCondition(AArch64CC::CondCode CC, StringRef Op, raw_string_ostream &OS);
    void genRdTwoOperandsAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Carry, bool Flags);
    void genRdImmAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Sticky);
    void genPCImmCondAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Cond);
    void genRtAddr(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, AccessSize Size, bool Scaled, bool Store);
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

unsigned getNZCVFlagsUsed(AArch64CC::CondCode CC) {
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

void AArch64HBNGAnnotate::storeAnnotation(StringRef &annotation, LLVMContext &Ctx) {
  Metadata *OpData[] = { MDString::get(Ctx, annotation) };
  Annotations.push_back(MDTuple::get(Ctx, OpData)); // Store metadata in vector
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

void AArch64HBNGAnnotate::genPCImmCondAnnotation(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, bool Cond) {
  // Use a raw_string_ostream to format the string.
  std::string FormatString;
  raw_string_ostream OS(FormatString);

  int ImmIndex = Cond ? 1 : 0;

  // TODO: Note that the ImmIndex now is not defined yet and refers to another basic block
  // While it might not be needed (tagging code memory is useful for self-modifying code)
  // it should be happening after the branch resolution (in the MCInst layer) if needed
  OS << "PC <- [<PC> + ";
  printStrOperand(MI.getOperand(ImmIndex), OS);
  OS << "] " << Op << " ";
  printStrOperand(MI.getOperand(ImmIndex), OS);
  if (Cond) {
    AArch64CC::CondCode CC = static_cast<AArch64CC::CondCode>(MI.getOperand(0).getImm());
    printCondition(CC, Op, OS);
  }
  OS << " " << Op << " PC";

  StringRef Result(FormatString);
  LLVM_DEBUG(dbgs() << Result << "\n");
  storeAnnotation(Result, Ctx);
}

void AArch64HBNGAnnotate::genRtAddr(MachineInstr &MI, LLVMContext &Ctx, StringRef Op, AccessSize Size, bool Scaled, bool Store) {
  // Use a raw_string_ostream to format the string.
  std::string FormatStringAddress;
  raw_string_ostream OSaddr(FormatStringAddress);


  std::string FormatString;
  raw_string_ostream OS(FormatString);

  uint64_t TSFlags = MI.getDesc().TSFlags;


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

  OSaddr << "[<";
  printStrOperand(MI.getOperand(1), OSaddr);
  OSaddr << "> + ";
  // TODO: Scale factor is not always 4
  if (Scaled) {
    OSaddr << " 4*";
  }
  if (MI.getOperand(2).isReg()) {
    OSaddr << "<";
    printStrOperand(MI.getOperand(2), OSaddr);

    AArch64_AM::ShiftExtendType SET = AArch64_AM::getMemExtendType(MI.getOperand(3).getImm());
    bool DoShift = AArch64_AM::getMemDoShift(MI.getOperand(4).getImm());
    OSaddr << "> " << AArch64_AM::getShiftExtendName(SET)<< " ";;
    if (DoShift) {
      OSaddr << "2";
    } else {
      OSaddr << "0";
    }
  } else {
    printStrOperand(MI.getOperand(2), OSaddr);
  }
  OSaddr << "]_" << Size;

  // Output the annotation in the form:
  // Rt <- [<Rn> + 4*(<Rm> extend amount)] Op Rn Op Rm

  if (Store) {
    OS << FormatStringAddress << " <- ";
    printStrOperand(MI.getOperand(0), OS);
  } else {
    printStrOperand(MI.getOperand(0), OS);
    OS << " <- " << FormatStringAddress;
  }
  OS << " " << Op << " ";
  printStrOperand(MI.getOperand(1), OS);
  OS << " " << Op << " ";
  printStrOperand(MI.getOperand(2), OS);

  StringRef Result(FormatString);
  LLVM_DEBUG(dbgs() << Result << "\n");
  storeAnnotation(Result, Ctx);
}

//===----------------------------------------------------------------------===//
// Main annotation generation function

void AArch64HBNGAnnotate::generateAnnotation(MachineInstr &MI, LLVMContext &Ctx) {
  std::string Unsupported;
  raw_string_ostream OS(Unsupported);
  OS << "Unsupported: " << TII->getName(MI.getOpcode());
  StringRef unsup(Unsupported);

  if (TII->isGPRZero(MI)) {
    genZero(MI, Ctx);
    return;
  }

  switch (MI.getOpcode()) {
    default:
        storeAnnotation(unsup, Ctx);
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
    //=== LOGICAL ===//
    case AArch64::ANDXrr: case AArch64::ANDXrs:
    case AArch64::ANDWrr: case AArch64::ANDWrs:
    case AArch64::BICXrr: case AArch64::BICXrs:
    case AArch64::BICWrr: case AArch64::BICWrs:
    case AArch64::EORXrr: case AArch64::EORXrs:
    case AArch64::EORWrr: case AArch64::EORWrs:
    case AArch64::EONXrr: case AArch64::EONXrs:
    case AArch64::EONWrr: case AArch64::EONWrs:
    case AArch64::ORNXrr: case AArch64::ORNXrs:
    case AArch64::ORNWrr: case AArch64::ORNWrs:
    case AArch64::ORRXrr: case AArch64::ORRXrs:
    case AArch64::ORRWrr: case AArch64::ORRWrs:
      genRdTwoOperandsAnnotation(MI, Ctx, "log", /*Carry=*/false, /*Flags=*/false);
      break;
    case AArch64::ANDSXrr: case AArch64::ANDSXrs:
    case AArch64::ANDSWrr: case AArch64::ANDSWrs:
    case AArch64::BICSXrr: case AArch64::BICSXrs:
    case AArch64::BICSWrr: case AArch64::BICSWrs:
      genRdTwoOperandsAnnotation(MI, Ctx, "log", /*Carry=*/false, /*Flags=*/true);
      break;
    //=== SHIFTS ===//
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
      genPCImmCondAnnotation(MI, Ctx, "bra", /*Cond=*/false);
      break;
    case AArch64::Bcc:
      genPCImmCondAnnotation(MI, Ctx, "bra", /*Cond=*/true);
      break;
    //=== BRANCH&LINK ===//
    //=== LOAD ===//
    // TODO: Other addressing modes
    case AArch64::LDRWroW:  case AArch64::LDRSWroW:
    case AArch64::LDRWroX: case AArch64::LDRSWroX:
      genRtAddr(MI, Ctx, "loa", WORD, /*Scaled=*/true, false);
      break;
    case AArch64::LDRWui: case AArch64::LDRSWui:
      genRtAddr(MI, Ctx, "loa", WORD, /*Scaled=*/true, false);
      break;
    case AArch64::LDRQui:
      genRtAddr(MI, Ctx, "loa", QUAD, /*Scaled=*/true, false);
      break;
    case AArch64::LDURWi: case AArch64::LDURSWi:
      genRtAddr(MI, Ctx, "loa", WORD, /*Scaled=*/false, false);
      break;
    case AArch64::LDRWpost: case AArch64::LDRSWpost:
    case AArch64::LDRWpre: case AArch64::LDRSWpre:
      break;
    //=== STORE ===//
    // TODO: Other addressing modes
    case AArch64::STRWroW: case AArch64::STRWroX:
      genRtAddr(MI, Ctx, "sto", WORD, /*Scaled=*/true, /*Store=*/true);
      break;
    case AArch64::STRWui: case AArch64::STRSui:
      genRtAddr(MI, Ctx, "sto", WORD, /*Scaled=*/true, /*Store=*/true);
      break;
    case AArch64::STRQui:
      genRtAddr(MI, Ctx, "sto", QUAD, /*Scaled=*/true, /*Store=*/true);
      break;
    case AArch64::STURWi:
      genRtAddr(MI, Ctx, "sto", WORD, /*Scaled=*/false, /*Store=*/true);
      break;

  }
}

//===----------------------------------------------------------------------===//
// Main loop

bool AArch64HBNGAnnotate::runOnMachineFunction(MachineFunction &MF) {
    LLVM_DEBUG(dbgs() << "***** HBNG Annotations *****\n");
    // Initialize subtarget information
    auto &STI = MF.getSubtarget<AArch64Subtarget>();
    TII = STI.getInstrInfo();
    TRI = MF.getSubtarget().getRegisterInfo();
    MRI = &MF.getRegInfo();
    // Get the context and module
    Module *M = MF.getFunction().getParent();
    LLVMContext &Ctx = MF.getFunction().getContext();

    // Attach collected data as module metadata, avoids polluting global symbols
    // and will be extracted at the emission stage and added to the binary.
    NamedMDNode *NMD = M->getOrInsertNamedMetadata("hbng_annotation_info");

    LLVM_DEBUG(dbgs() << "func: " << MF.getName() << "\n");
    for (auto &MBB : MF) {
      LLVM_DEBUG(dbgs() << "bblock: " << MBB.getName() << "\n");
      for (auto &MI : MBB) {
        unsigned int opcode = MI.getOpcode();
        StringRef mnemonic = TII->getName(opcode);
        LLVM_DEBUG(dbgs() << "instr: " << mnemonic << "\n");
        generateAnnotation(MI, Ctx);
      }
      // Adding an end instruction in the annotations
      std::string endAnnotation = "END";
      StringRef endRef(endAnnotation);
      storeAnnotation(endRef, Ctx);
    }

    for (auto &Annotation : Annotations) {
      NMD->addOperand(Annotation);
    }

    // Notify if the content has changed
    return false;
}


// Factory function used by AArch64TargetMachine to add the pass to
// the passmanager.
FunctionPass *llvm::createAArch64HBNGAnnotatePass() {
  return new AArch64HBNGAnnotate();
}