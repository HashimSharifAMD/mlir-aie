// (c) Copyright 2019 Xilinx Inc. All Rights Reserved.

#include "mlir/IR/Attributes.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Location.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Translation.h"
#include "AIEDialect.h"
#include "AIENetlistAnalysis.h"

using namespace mlir;
using namespace xilinx;
using namespace xilinx::AIE;

typedef std::pair<WireBundle, int> PortTy;
typedef std::pair<PortTy, PortTy> ConnectTy;

template <typename MyOp>
struct AIEOpRemoval : public OpConversionPattern<MyOp> {
  using OpConversionPattern<MyOp>::OpConversionPattern;
  ModuleOp &module;

  AIEOpRemoval(MLIRContext *context, ModuleOp &m,
    PatternBenefit benefit = 1
  ) : OpConversionPattern<MyOp>(context, benefit),
    module(m) {}

  LogicalResult matchAndRewrite(MyOp op, ArrayRef<Value> operands,
                                ConversionPatternRewriter &rewriter) const override {
    Operation *Op = op.getOperation();

    rewriter.eraseOp(Op);
    return success();
  }
};

int getAvailableDestChannel(
  SmallVector<ConnectTy, 8> &connects,
  WireBundle destBundle) {

  if (connects.size() == 0)
    return 0;

  int numChannels;

  if (destBundle == WireBundle::North)
    numChannels = 6;
  else if (destBundle == WireBundle::South ||
           destBundle == WireBundle::East  ||
           destBundle == WireBundle::West)
    numChannels = 4;
  else
    numChannels = 2;

  int availableChannel = -1;

  for (int i = 0; i < numChannels; i++) {
    PortTy port = std::make_pair(destBundle, i);
    SmallVector<PortTy, 8> ports;
    for (auto connect : connects)
      ports.push_back(connect.second);

    if (std::find(ports.begin(), ports.end(), port) == ports.end())
      return i;
  }

  return -1;
}

void build_route(int xSrc, int ySrc, int dX, int dY,
  WireBundle sourceBundle, int sourceChannel,
  WireBundle destBundle, int destChannel,
  Operation *herdOp,
  DenseMap<std::pair<Operation *, std::pair<int, int>>, SmallVector<ConnectTy, 8>> &switchboxes) {

  int xCnt = 0;
  int yCnt = 0;

  int xCur = xSrc;
  int yCur = ySrc;
  WireBundle curBundle;
  int curChannel;
  int xLast, yLast;
  WireBundle lastBundle;
  PortTy lastPort = std::make_pair(sourceBundle, sourceChannel);

  int xDest = xSrc + dX;
  int yDest = ySrc + dY;

  SmallVector<std::pair<int, int>, 4> congestion;

  llvm::dbgs() << "Build route: " << xSrc << " " << ySrc << " --> " << xDest << " " << yDest << '\n';
  // traverse horizontally, then vertically
  while (!((xCur == xDest) && (yCur == yDest))) {
    llvm::dbgs() << "coord " << xCur << " " << yCur << '\n';

    auto curCoord = std::make_pair(xCur, yCur);
    xLast = xCur;
    yLast = yCur;

    SmallVector<WireBundle, 4> moves;

    if (xCur < xDest)
      moves.push_back(WireBundle::East);
    if (xCur > xDest)
      moves.push_back(WireBundle::West);
    if (yCur < yDest)
      moves.push_back(WireBundle::North);
    if (yCur > yDest)
      moves.push_back(WireBundle::South);

    if (std::find(moves.begin(), moves.end(), WireBundle::East) == moves.end())
      moves.push_back(WireBundle::East);
    if (std::find(moves.begin(), moves.end(), WireBundle::West) == moves.end())
      moves.push_back(WireBundle::West);
    if (std::find(moves.begin(), moves.end(), WireBundle::North) == moves.end())
      moves.push_back(WireBundle::North);
    if (std::find(moves.begin(), moves.end(), WireBundle::South) == moves.end())
      moves.push_back(WireBundle::South);

    for (unsigned i = 0; i < moves.size(); i++) {
      WireBundle move = moves[i];
      curChannel = getAvailableDestChannel(
                     switchboxes[std::make_pair(herdOp, curCoord)], move);
      if (curChannel == -1)
        continue;

      if (move == lastBundle)
        continue;

      if (move == WireBundle::East) {
        xCur = xCur + 1;
        yCur = yCur;
      } else if (move == WireBundle::West) {
        xCur = xCur - 1;
        yCur = yCur;
      } else if (move == WireBundle::North) {
        xCur = xCur;
        yCur = yCur + 1;
      } else if (move == WireBundle::South) {
        xCur = xCur;
        yCur = yCur - 1;
      }

      if (std::find(congestion.begin(), congestion.end(), std::make_pair(xCur, yCur)) != congestion.end())
        continue;

      curBundle = move;
      lastBundle = (move == WireBundle::East)  ? WireBundle::West :
                   (move == WireBundle::West)  ? WireBundle::East :
                   (move == WireBundle::North) ? WireBundle::South :
                   (move == WireBundle::South) ? WireBundle::North : lastBundle;
      break;
    }

    assert(curChannel >= 0 && "Could not find available destination port!");

    if (curChannel == -1) {
      congestion.push_back(std::make_pair(xLast, yLast)); // this switchbox is congested
      switchboxes[std::make_pair(herdOp, curCoord)].pop_back(); // back up, remove the last connection
    } else {
      llvm::dbgs() << "[" << stringifyWireBundle(lastPort.first) << " : " << lastPort.second << "], "
                      "[" << stringifyWireBundle(curBundle) << " : " << curChannel << "]\n";

      PortTy curPort = std::make_pair(curBundle, curChannel);
      switchboxes[std::make_pair(herdOp, curCoord)].push_back(std::make_pair(lastPort, curPort));
      lastPort = std::make_pair(lastBundle, curChannel);
    }
  }

  switchboxes[std::make_pair(herdOp, std::make_pair(xCur, yCur))].push_back(
    std::make_pair(lastPort, std::make_pair(destBundle, destChannel)));
}

struct AIEHerdRoutingPass : public PassWrapper<AIEHerdRoutingPass, OperationPass<ModuleOp>> {
  void runOnOperation() override {

    ModuleOp m = getOperation();
    OpBuilder builder(m.getBody()->getTerminator());

    SmallVector<HerdOp, 4> herds;
    SmallVector<Operation *, 4> placeOps;
    SmallVector<Operation *, 4> routeOps;
    DenseMap<std::pair<Operation *, Operation *>, std::pair<int, int>> distances;
    SmallVector<std::pair<std::pair<int, int>, std::pair<int, int>>, 4> routes;
    DenseMap<std::pair<Operation *, std::pair<int, int>>, SmallVector<ConnectTy, 8>> switchboxes;

    for (auto herd : m.getOps<HerdOp>()) {
      herds.push_back(herd);
    }

    for (auto placeOp : m.getOps<PlaceOp>()) {
      placeOps.push_back(placeOp);
      Operation *sourceHerd = placeOp.sourceHerd().getDefiningOp();
      Operation *destHerd = placeOp.destHerd().getDefiningOp();
      int distX = placeOp.getDistXValue();
      int distY = placeOp.getDistYValue();
      distances[std::make_pair(sourceHerd, destHerd)] = std::make_pair(distX, distY);
    }

    for (auto routeOp : m.getOps<RouteOp>()) {
      routeOps.push_back(routeOp);

      AIE::SelectOp sourceHerds = dyn_cast<AIE::SelectOp>(routeOp.sourceHerds().getDefiningOp());
      AIE::SelectOp destHerds = dyn_cast<AIE::SelectOp>(routeOp.destHerds().getDefiningOp());
      WireBundle sourceBundle = routeOp.sourceBundle();
      WireBundle destBundle = routeOp.destBundle();
      int sourceChannel = routeOp.getSourceChannelValue();
      int destChannel = routeOp.getDestChannelValue();

      HerdOp sourceHerd = dyn_cast<HerdOp>(sourceHerds.startHerd().getDefiningOp());
      IterOp sourceIterX = dyn_cast<IterOp>(sourceHerds.iterX().getDefiningOp());
      IterOp sourceIterY = dyn_cast<IterOp>(sourceHerds.iterY().getDefiningOp());

      HerdOp destHerd = dyn_cast<HerdOp>(destHerds.startHerd().getDefiningOp());
      IterOp destIterX = dyn_cast<IterOp>(destHerds.iterX().getDefiningOp());
      IterOp destIterY = dyn_cast<IterOp>(destHerds.iterY().getDefiningOp());

      int sourceStartX  = sourceIterX.getStartValue();
      int sourceEndX    = sourceIterX.getEndValue();
      int sourceStrideX = sourceIterX.getStrideValue();
      int sourceStartY  = sourceIterY.getStartValue();
      int sourceEndY    = sourceIterY.getEndValue();
      int sourceStrideY = sourceIterY.getStrideValue();

      int destStartX  = destIterX.getStartValue();
      int destEndX    = destIterX.getEndValue();
      int destStrideX = destIterX.getStrideValue();
      int destStartY  = destIterY.getStartValue();
      int destEndY    = destIterY.getEndValue();
      int destStrideY = destIterY.getStrideValue();

      assert(distances.count(std::make_pair(sourceHerd, destHerd)) == 1);

      std::pair<int, int> distance = distances[std::make_pair(sourceHerd, destHerd)];
      int distX = distance.first;
      int distY = distance.second;
      int xStride, yStride;

      for (int xSrc = sourceStartX; xSrc < sourceEndX; xSrc += sourceStrideX) {
        for (int ySrc = sourceStartY; ySrc < sourceEndY; ySrc += sourceStrideY) {
          for (int xDst = destStartX; xDst < destEndX; xDst += destStrideX) {
            for (int yDst = destStartY; yDst < destEndY; yDst += destStrideY) {
              // Build route (x0, y0) --> (x1, y1)
              int x0 = xSrc;
              int y0 = ySrc;
              int x1 = xDst;
              int y1 = yDst;
              if (destIterX == sourceIterX)
                x1 = x0;
              if (destIterY == sourceIterY)
                y1 = x0;
              if (destIterX == sourceIterY)
                x1 = y0;
              if (destIterY == sourceIterY)
                y1 = y0;

              int dX = distX + x1 - x0;
              int dY = distY + y1 - y0;

              auto route = std::make_pair(std::make_pair(x0, y0), std::make_pair(dX, dY));
              if (std::find(routes.begin(), routes.end(), route) != routes.end())
                continue;

              build_route(x0, y0, dX, dY,
                sourceBundle, sourceChannel,
                destBundle, destChannel,
                sourceHerd,
                switchboxes);

              routes.push_back(route);
            }
          }
        }
      }
    }

    for (auto swboxCfg : switchboxes) {
      Operation *herdOp = swboxCfg.first.first;
      int x = swboxCfg.first.second.first;
      int y = swboxCfg.first.second.second;
      auto connects = swboxCfg.second;
      HerdOp herd = dyn_cast<HerdOp>(herdOp);

      builder.setInsertionPoint(m.getBody()->getTerminator());

      IterOp iterx = builder.create<IterOp>(builder.getUnknownLoc(), x, x + 1, 1);
      IterOp itery = builder.create<IterOp>(builder.getUnknownLoc(), y, y + 1, 1);
      AIE::SelectOp sel = builder.create<AIE::SelectOp>(builder.getUnknownLoc(), herd, iterx, itery);
      SwitchboxOp swbox = builder.create<SwitchboxOp>(builder.getUnknownLoc(), sel);
      swbox.ensureTerminator(swbox.connections(), builder, builder.getUnknownLoc());
      Block &b = swbox.connections().front();
      builder.setInsertionPoint(b.getTerminator());

      for (auto connect : connects) {
        PortTy sourcePort = connect.first;
        PortTy destPort = connect.second;
        WireBundle sourceBundle = sourcePort.first;
        int sourceChannel = sourcePort.second;
        WireBundle destBundle = destPort.first;
        int destChannel = destPort.second;

        builder.create<ConnectOp>(builder.getUnknownLoc(),
                                  sourceBundle, sourceChannel,
                                  destBundle, destChannel);

      }
    }

    ConversionTarget target(getContext());

    OwningRewritePatternList patterns;
    patterns.insert<AIEOpRemoval<PlaceOp>,
                    AIEOpRemoval<RouteOp>
                   >(m.getContext(), m);

    if (failed(applyPartialConversion(m, target, patterns)))
      signalPassFailure();
  }
};

void xilinx::AIE::registerAIEHerdRoutingPass() {
    PassRegistration<AIEHerdRoutingPass>(
      "aie-herd-routing",
      "Lowering herds with place and route ops to AIE cores, mems, and switchboxes");
}
