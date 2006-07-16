//===-- ARMISelDAGToDAG.cpp - A dag to dag inst selector for ARM ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Chris Lattner and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the ARM target.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMTargetMachine.h"
#include "llvm/CallingConv.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Intrinsics.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Support/Debug.h"
#include <iostream>
#include <set>
using namespace llvm;

namespace {
  class ARMTargetLowering : public TargetLowering {
  public:
    ARMTargetLowering(TargetMachine &TM);
    virtual SDOperand LowerOperation(SDOperand Op, SelectionDAG &DAG);
    virtual const char *getTargetNodeName(unsigned Opcode) const;
  };

}

ARMTargetLowering::ARMTargetLowering(TargetMachine &TM)
  : TargetLowering(TM) {
  setOperationAction(ISD::RET, MVT::Other, Custom);
}

namespace llvm {
  namespace ARMISD {
    enum NodeType {
      // Start the numbering where the builting ops and target ops leave off.
      FIRST_NUMBER = ISD::BUILTIN_OP_END+ARM::INSTRUCTION_LIST_END,
      /// CALL - A direct function call.
      CALL
    };
  }
}

const char *ARMTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  default: return 0;
  case ARMISD::CALL:          return "ARMISD::CALL";
  }
}

// This transforms a ISD::CALL node into a
// callseq_star <- ARMISD:CALL <- callseq_end
// chain
static SDOperand LowerCALL(SDOperand Op, SelectionDAG &DAG) {
  SDOperand Chain    = Op.getOperand(0);
  unsigned CallConv  = cast<ConstantSDNode>(Op.getOperand(1))->getValue();
  assert(CallConv == CallingConv::C && "unknown calling convention");
  bool isVarArg      = cast<ConstantSDNode>(Op.getOperand(2))->getValue() != 0;
  assert(isVarArg == false && "VarArg not supported");
  bool isTailCall    = cast<ConstantSDNode>(Op.getOperand(3))->getValue() != 0;
  assert(isTailCall == false && "tail call not supported");
  SDOperand Callee   = Op.getOperand(4);
  unsigned NumOps    = (Op.getNumOperands() - 5) / 2;
  assert(NumOps == 0);

  // Count how many bytes are to be pushed on the stack. Initially
  // only the link register.
  unsigned NumBytes = 4;

  // Adjust the stack pointer for the new arguments...
  // These operations are automatically eliminated by the prolog/epilog pass
  Chain = DAG.getCALLSEQ_START(Chain,
                               DAG.getConstant(NumBytes, MVT::i32));

  std::vector<MVT::ValueType> NodeTys;
  NodeTys.push_back(MVT::Other);   // Returns a chain
  NodeTys.push_back(MVT::Flag);    // Returns a flag for retval copy to use.

  // If the callee is a GlobalAddress/ExternalSymbol node (quite common, every
  // direct call is) turn it into a TargetGlobalAddress/TargetExternalSymbol
  // node so that legalize doesn't hack it.
  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), Callee.getValueType());

  // If this is a direct call, pass the chain and the callee.
  assert (Callee.Val);
  std::vector<SDOperand> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);

  unsigned CallOpc = ARMISD::CALL;
  Chain = DAG.getNode(CallOpc, NodeTys, Ops);

  assert(Op.Val->getValueType(0) == MVT::Other);

  Chain = DAG.getNode(ISD::CALLSEQ_END, MVT::Other, Chain,
                      DAG.getConstant(NumBytes, MVT::i32));

  return Chain;
}

static SDOperand LowerRET(SDOperand Op, SelectionDAG &DAG) {
  SDOperand Copy;
  SDOperand Chain = Op.getOperand(0);
  switch(Op.getNumOperands()) {
  default:
    assert(0 && "Do not know how to return this many arguments!");
    abort();
  case 1: {
    SDOperand LR = DAG.getRegister(ARM::R14, MVT::i32);
    return DAG.getNode(ISD::BRIND, MVT::Other, Chain, LR);
  }
  case 3:
    Copy = DAG.getCopyToReg(Chain, ARM::R0, Op.getOperand(1), SDOperand());
    if (DAG.getMachineFunction().liveout_empty())
      DAG.getMachineFunction().addLiveOut(ARM::R0);
    break;
  }

  SDOperand LR = DAG.getRegister(ARM::R14, MVT::i32);

  //bug: the copy and branch should be linked with a flag so that the
  //scheduller can't move an instruction that destroys R0 in between them
  //return DAG.getNode(ISD::BRIND, MVT::Other, Copy, LR, Copy.getValue(1));

  return DAG.getNode(ISD::BRIND, MVT::Other, Copy, LR);
}

static SDOperand LowerFORMAL_ARGUMENT(SDOperand Op, SelectionDAG &DAG,
				      unsigned ArgNo) {
  MachineFunction &MF = DAG.getMachineFunction();
  MVT::ValueType ObjectVT = Op.getValue(ArgNo).getValueType();
  assert (ObjectVT == MVT::i32);
  SDOperand Root = Op.getOperand(0);
  SSARegMap *RegMap = MF.getSSARegMap();

  unsigned num_regs = 4;
  static const unsigned REGS[] = {
    ARM::R0, ARM::R1, ARM::R2, ARM::R3
  };

  if(ArgNo < num_regs) {
    unsigned VReg = RegMap->createVirtualRegister(&ARM::IntRegsRegClass);
    MF.addLiveIn(REGS[ArgNo], VReg);
    return DAG.getCopyFromReg(Root, VReg, MVT::i32);
  } else {
    // If the argument is actually used, emit a load from the right stack
      // slot.
    if (!Op.Val->hasNUsesOfValue(0, ArgNo)) {
      unsigned ArgOffset = (ArgNo - num_regs) * 4;

      MachineFrameInfo *MFI = MF.getFrameInfo();
      unsigned ObjSize = MVT::getSizeInBits(ObjectVT)/8;
      int FI = MFI->CreateFixedObject(ObjSize, ArgOffset);
      SDOperand FIN = DAG.getFrameIndex(FI, MVT::i32);
      return DAG.getLoad(ObjectVT, Root, FIN,
			 DAG.getSrcValue(NULL));
    } else {
      // Don't emit a dead load.
      return DAG.getNode(ISD::UNDEF, ObjectVT);
    }
  }
}

static SDOperand LowerFORMAL_ARGUMENTS(SDOperand Op, SelectionDAG &DAG) {
  std::vector<SDOperand> ArgValues;
  SDOperand Root = Op.getOperand(0);

  for (unsigned ArgNo = 0, e = Op.Val->getNumValues()-1; ArgNo != e; ++ArgNo) {
    SDOperand ArgVal = LowerFORMAL_ARGUMENT(Op, DAG, ArgNo);

    ArgValues.push_back(ArgVal);
  }

  bool isVarArg = cast<ConstantSDNode>(Op.getOperand(2))->getValue() != 0;
  assert(!isVarArg);

  ArgValues.push_back(Root);

  // Return the new list of results.
  std::vector<MVT::ValueType> RetVT(Op.Val->value_begin(),
                                    Op.Val->value_end());
  return DAG.getNode(ISD::MERGE_VALUES, RetVT, ArgValues);
}

SDOperand ARMTargetLowering::LowerOperation(SDOperand Op, SelectionDAG &DAG) {
  switch (Op.getOpcode()) {
  default:
    assert(0 && "Should not custom lower this!");
    abort();
  case ISD::FORMAL_ARGUMENTS:
    return LowerFORMAL_ARGUMENTS(Op, DAG);
  case ISD::CALL:
    return LowerCALL(Op, DAG);
  case ISD::RET:
    return LowerRET(Op, DAG);
  }
}

//===----------------------------------------------------------------------===//
// Instruction Selector Implementation
//===----------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
/// ARMDAGToDAGISel - ARM specific code to select ARM machine
/// instructions for SelectionDAG operations.
///
namespace {
class ARMDAGToDAGISel : public SelectionDAGISel {
  ARMTargetLowering Lowering;

public:
  ARMDAGToDAGISel(TargetMachine &TM)
    : SelectionDAGISel(Lowering), Lowering(TM) {
  }

  void Select(SDOperand &Result, SDOperand Op);
  virtual void InstructionSelectBasicBlock(SelectionDAG &DAG);
  bool SelectAddrRegImm(SDOperand N, SDOperand &Offset, SDOperand &Base);

  // Include the pieces autogenerated from the target description.
#include "ARMGenDAGISel.inc"
};

void ARMDAGToDAGISel::InstructionSelectBasicBlock(SelectionDAG &DAG) {
  DEBUG(BB->dump());

  DAG.setRoot(SelectRoot(DAG.getRoot()));
  assert(InFlightSet.empty() && "ISel InFlightSet has not been emptied!");
  CodeGenMap.clear();
  HandleMap.clear();
  ReplaceMap.clear();
  DAG.RemoveDeadNodes();

  ScheduleAndEmitDAG(DAG);
}

//register plus/minus 12 bit offset
bool ARMDAGToDAGISel::SelectAddrRegImm(SDOperand N, SDOperand &Offset,
				    SDOperand &Base) {
  Offset = CurDAG->getTargetConstant(0, MVT::i32);
  if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(N)) {
    Base = CurDAG->getTargetFrameIndex(FI->getIndex(), N.getValueType());
  }
  else
    Base = N;
  return true;      //any address fits in a register
}

void ARMDAGToDAGISel::Select(SDOperand &Result, SDOperand Op) {
  SDNode *N = Op.Val;

  switch (N->getOpcode()) {
  default:
    SelectCode(Result, Op);
    break;
  }
}

}  // end anonymous namespace

/// createARMISelDag - This pass converts a legalized DAG into a
/// ARM-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createARMISelDag(TargetMachine &TM) {
  return new ARMDAGToDAGISel(TM);
}
