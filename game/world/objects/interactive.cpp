#include "interactive.h"

#include <Tempest/Painter>
#include <Tempest/Log>

#include "game/serialize.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/pose.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "utils/fileext.h"
#include "utils/dbgpainter.h"

Interactive::Interactive(Vob* parent, World &world, ZenLoad::zCVobData& vob, bool startup)
  : Vob(parent,world,vob,startup) {
  vobType       = vob.vobType;
  vobName       = std::move(vob.vobName);
  focName       = std::move(vob.oCMOB.focusName);
  bbox[0]       = vob.bbox[0];
  bbox[1]       = vob.bbox[1];
  owner         = std::move(vob.oCMOB.owner);
  focOver       = vob.oCMOB.focusOverride;
  showVisual    = vob.showVisual;

  stateNum      = vob.oCMobInter.stateNum;
  triggerTarget = std::move(vob.oCMobInter.triggerTarget);
  useWithItem   = std::move(vob.oCMobInter.useWithItem);
  conditionFunc = std::move(vob.oCMobInter.conditionFunc);
  onStateFunc   = std::move(vob.oCMobInter.onStateFunc);
  rewind        = std::move(vob.oCMobInter.rewind);

  for(auto& i:owner)
    i = char(std::toupper(i));

  if(vobType==ZenLoad::zCVobData::VT_oCMobContainer ||
     vobType==ZenLoad::zCVobData::VT_oCMobDoor) {
    locked      = vob.oCMobLockable.locked;
    keyInstance = std::move(vob.oCMobLockable.keyInstance);
    pickLockStr = std::move(vob.oCMobLockable.pickLockStr);
    }

  if(isContainer()) {
    auto items  = std::move(vob.oCMobContainer.contains);
    if(items.size()>0) {
      char* it = &items[0];
      for(auto i=it;;++i) {
        if(*i==','){
          *i='\0';
          implAddItem(it);
          it=i+1;
          }
        else if(*i=='\0'){
          implAddItem(it);
          it=i+1;
          break;
          }
        }
      }
    }

  setVisual(vob);
  mdlVisual = std::move(vob.visual);

  world.addInteractive(this);
  }

void Interactive::load(Serialize &fin) {
  if(fin.version()<25 && vobType==ZenLoad::zCVobData::VT_oCMobFire) {
    // mob fire wasn't here until v25
    Vob::load(fin);
    return;
    }

  Tempest::Matrix4x4 pos;
  uint8_t vt=0;
  fin.read(vt,vobName,focName,mdlVisual);
  vobType = ZenLoad::zCVobData::EVobType(vt);
  fin.read(bbox[0].x,bbox[0].y,bbox[0].z,bbox[1].x,bbox[1].y,bbox[1].z,owner);
  if(fin.version()>=2)
    fin.read(focOver);
  if(fin.version()>=6)
    fin.read(showVisual);

  fin.read(stateNum,triggerTarget,useWithItem,conditionFunc,onStateFunc);
  fin.read(locked,keyInstance,pickLockStr);
  invent.load(*this,world,fin);
  fin.read(pos,state,reverseState,loopState);
  if(fin.version()>=12)
    fin.read(isLockCracked);

  setGlobalTransform(pos);
  // setVisual(mdlVisual);
  visual.load(fin,*this);
  visual.setObjMatrix(transform());

  uint32_t sz=0;
  fin.read(sz);
  for(size_t i=0;i<sz;++i){
    std::string name;
    Npc*        user=nullptr;
    int32_t     userState=0; // unused
    bool        attachMode = false;
    bool        started    = false;

    fin.read(name,user,userState,attachMode);
    if(fin.version()>=13)
      fin.read(started);

    for(auto& i:attPos)
      if(i.name==name) {
        i.user       = user;
        i.attachMode = attachMode;
        i.started    = started;
        if(fin.version()<24 && i.user!=nullptr)
          i.user->setInteraction(this,true);
        }
    }
  }

void Interactive::save(Serialize &fout) const {
  fout.write(uint8_t(vobType),vobName,focName,mdlVisual);
  fout.write(bbox[0].x,bbox[0].y,bbox[0].z,bbox[1].x,bbox[1].y,bbox[1].z);
  fout.write(owner,focOver,showVisual);
  fout.write(stateNum,triggerTarget,useWithItem,conditionFunc,onStateFunc);
  fout.write(locked,keyInstance,pickLockStr);
  invent.save(fout);
  fout.write(transform(),state,reverseState,loopState,isLockCracked);
  visual.save(fout,*this);

  fout.write(uint32_t(attPos.size()));
  for(auto& i:attPos) {
    int32_t userState=0; // unused
    fout.write(i.name,i.user,userState,i.attachMode,i.started);
    }
  }

void Interactive::postValidate() {
  for(auto& i:attPos)
    if(i.user!=nullptr && i.user->interactive()!=this)
      i.user = nullptr;
  }

void Interactive::resetPositionToTA() {
  if(isAvailable()) {
    visual.startAnimAndGet("S_S0",world.tickCount(),true);
    setState(0);
    }
  }

void Interactive::setVisual(ZenLoad::zCVobData& vob) {
  visual.setVisual(vob,world);
  visual.setObjMatrix(transform());
  animChanged = true;
  if(auto mesh = visual.protoMesh()) {
    attPos.resize(mesh->pos.size());
    for(size_t i=0;i<attPos.size();++i){
      attPos[i].name = mesh->pos[i].name;
      attPos[i].pos  = mesh->pos[i].transform;
      attPos[i].node = mesh->pos[i].node;
      }
    }
  setAnim(Interactive::Active); // setup default anim
  }

void Interactive::updateAnimation() {
  if(visual.updateAnimation(nullptr,world))
    animChanged = true;
  }

void Interactive::tick(uint64_t dt) {
  visual.processLayers(world);

  if(animChanged) {
    visual.syncPhysics();
    animChanged = false;
    }

  Pos* p = nullptr;
  for(auto& i:attPos) {
    if(i.user!=nullptr) {
      p = &i;
      }
    }

  if(world.tickCount()<waitAnim)
    return;

  if(p==nullptr) {
    if(rewind && state!=-1) {
      if(!setAnim(nullptr,Anim::Out))
        return;
      setState(std::max(0,state-1));
      }
    return;
    }

  if(p->user==nullptr && (state==0 && !p->attachMode))
    return;
  if(p->user==nullptr && (state==stateNum && p->attachMode))
    return;
  implTick(*p,dt);
  }

void Interactive::implTick(Pos& p, uint64_t /*dt*/) {
  if(p.user==nullptr)
    return;

  Npc& npc = *p.user;
  if(!p.started) {
    // STAND -> S0
    auto sq = npc.setAnimAngGet(Npc::Anim::InteractFromStand,false);
    uint64_t t = sq==nullptr ? 0 : uint64_t(sq->totalTime());
    waitAnim   = world.tickCount()+t;
    p.started  = sq!=nullptr;
    return;
    }

  const bool atach = (p.attachMode^reverseState);

  if(!loopState) {
    if(stateNum==state && atach) {
      invokeStateFunc(npc);
      loopState = true;
      }
    if(0==state && !p.attachMode) {
      invokeStateFunc(npc);
      loopState = true;
      }
    }

  if(!atach && state==0) {
    implQuitInteract(p);
    return;
    }

  if(needToLockpick(npc)) {
    if(p.attachMode) {
      npc.world().sendPassivePerc(npc,npc,npc,Npc::PERC_ASSESSUSEMOB);
      return; // chest is locked - need to crack lock first
      }
    }

  if((p.attachMode^reverseState) && state==stateNum){
    // if(!setAnim(&npc,Anim::Active))
    //   return;
    }
  else if(p.attachMode) {
    if(!setAnim(&npc,Anim::In))
      return;
    }
  else {
    if(!setAnim(&npc,Anim::Out))
      return;
    }

  if(state==0 && p.attachMode) {
    npc.world().sendPassivePerc(npc,npc,npc,Npc::PERC_ASSESSUSEMOB);
    emitTriggerEvent();
    }

  if(npc.isPlayer() && !loopState) {
    invokeStateFunc(npc);
    }

  int prev = state;
  if(atach) {
    setState(std::min(stateNum,state+1));
    } else {
    setState(std::max(0,state-1));
    }
  loopState = (prev==state);
  }

void Interactive::implQuitInteract(Interactive::Pos &p) {
  if(p.user==nullptr)
    return;
  Npc& npc = *p.user;
  if(!(npc.isPlayer() && (!npc.world().aiIsDlgFinished() || !npc.isAiQueueEmpty()))) {
    const Animation::Sequence* sq = nullptr;
    if(state==0) {
      // S0 -> STAND
      sq = npc.setAnimAngGet(Npc::Anim::InteractToStand,false);
      }
    if(sq==nullptr && !npc.setAnim(Npc::Anim::Idle))
      return;
    npc.quitIneraction();
    p.user      = nullptr;
    loopState   = false;
    }
  }

const std::string &Interactive::tag() const {
  return vobName;
  }

const std::string& Interactive::focusName() const {
  return focName;
  }

bool Interactive::checkMobName(const char* dest) const {
  const char* scheme=schemeName();
  if(strcmp(scheme,dest)==0)
    return true;
  return false;
  }

const std::string &Interactive::ownerName() const {
  return owner;
  }

bool Interactive::overrideFocus() const {
  return focOver;
  }

Tempest::Vec3 Interactive::displayPosition() const {
  auto p = position();
  return {p.x,bbox[1].y,p.z};
  }

const char *Interactive::displayName() const {
  if(focName.empty())
    return "";

  const char* strId=focName.c_str();
  char buf[256] = {};
  if(world.getSymbolIndex(strId)==size_t(-1)) {
    std::snprintf(buf,sizeof(buf),"MOBNAME_%s",strId);
    strId = buf;
    }

  if(world.getSymbolIndex(strId)==size_t(-1)) {
    return "";
    }

  auto& s=world.getSymbol(strId);
  const char* txt = s.getString(0).c_str();
  if(std::strlen(txt)==0)
    txt="";
  return txt;
  }

bool Interactive::setMobState(const char* scheme, int32_t st) {
  const bool ret = Vob::setMobState(scheme,st);
  if(state==st)
    return true;

  if(std::strcmp(schemeName(),scheme)!=0)
    return ret;

  char buf[256]={};
  std::snprintf(buf,sizeof(buf),"S_S%d",st);
  if(visual.startAnimAndGet(buf,world.tickCount())!=nullptr ||
     !visual.isAnimExist(buf)) {
    setState(st);
    return ret;
    }
  return false;
  }

void Interactive::invokeStateFunc(Npc& npc) {
  if(onStateFunc.empty() || state<0)
    return;
  if(loopState)
    return;
  char func[256]={};
  std::snprintf(func,sizeof(func),"%s_S%d",onStateFunc.c_str(),state);

  auto& sc = npc.world().script();
  sc.useInteractive(npc.handle(),func);
  }

void Interactive::emitTriggerEvent() const {
  if(triggerTarget.empty())
    return;
  const TriggerEvent evt(triggerTarget,vobName,TriggerEvent::T_Activate);
  world.triggerEvent(evt);
  }

const char* Interactive::schemeName() const {
  if(auto mesh = visual.protoMesh())
    return mesh->scheme.c_str();
  Tempest::Log::i("unable to recognize mobsi{",focName,", ",mdlVisual,"}");
  return "";
  }

const char* Interactive::posSchemeName() const {
  for(auto& i:attPos)
    if(i.user!=nullptr) {
      return i.posTag();
      }
  return nullptr;
  }

bool Interactive::isContainer() const {
  return vobType==ZenLoad::zCVobData::VT_oCMobContainer;
  }

bool Interactive::needToLockpick(const Npc& pl) const {
  const size_t keyInst = keyInstance.empty() ? size_t(-1) : world.getSymbolIndex(keyInstance.c_str());
  if(keyInst!=size_t(-1) && pl.inventory().itemCount(keyInst)>0)
    return false;
  return !(pickLockStr.empty() || isLockCracked);
  }

Inventory &Interactive::inventory()  {
  return invent;
  }

uint32_t Interactive::stateMask() const {
  const char* s = schemeName();
  return world.script().schemeToBodystate(s);
  }

bool Interactive::canSeeNpc(const Npc& npc, bool freeLos) const {
  for(auto& i:attPos){
    auto pos = nodePosition(npc,i);
    if(npc.canSeeNpc(pos.x,pos.y,pos.z,freeLos))
      return true;
    }

  // graves
  if(attPos.size()==0){
    auto pos = position();

    float x = pos.x;
    float y = pos.y;
    float z = pos.z;
    if(npc.canSeeNpc(x,y,z,freeLos))
      return true;
    }
  return false;
  }

Tempest::Vec3 Interactive::nearestPoint(const Npc& to) {
  if(auto p = findNearest(to))
    return worldPos(*p);
  return Tempest::Vec3();
  }

Interactive::Pos* Interactive::findNearest(const Npc& to) {
  float dist = 0;
  Pos*  p    = nullptr;
  for(auto& i:attPos) {
    if(i.user || !i.isAttachPoint())
      continue;
    float d = qDistanceTo(to,i);
    if(d<dist || p==nullptr) {
      p    = &i;
      dist = d;
      }
    }
  return p;
  }

void Interactive::implAddItem(char *name) {
  char* sep = std::strchr(name,':');
  if(sep!=nullptr) {
    *sep='\0';++sep;
    long count = std::strtol(sep,nullptr,10);
    if(count>0)
      invent.addItem(name,size_t(count),world);
    } else {
    invent.addItem(name,1,world);
    }
  }

void Interactive::autoDettachNpc() {
  for(auto& i:attPos)
    if(i.user!=nullptr) {
      if(i.user->world().aiIsDlgFinished())
        i.user->setInteraction(nullptr);
      }
  }

bool Interactive::checkUseConditions(Npc& npc) {
  const bool isPlayer = npc.isPlayer();

  auto& sc = npc.world().script();

  if(isPlayer) {
    const size_t ItKE_lockpick  = world.getSymbolIndex("ItKE_lockpick");
    const size_t lockPickCnt    = npc.inventory().itemCount(ItKE_lockpick);
    const bool   canLockPick    = (npc.talentSkill(Npc::TALENT_PICKLOCK)!=0 && lockPickCnt>0);

    const size_t keyInst        = keyInstance.empty() ? size_t(-1) : world.getSymbolIndex(keyInstance.c_str());
    const bool   needToPicklock = (pickLockStr.size()>0);

    if(keyInst!=size_t(-1) && npc.hasItem(keyInst)>0)
      return true;
    if((canLockPick || isLockCracked) && needToPicklock)
      return true;

    if(keyInst!=size_t(-1) && needToPicklock) { // key+lockpick
      sc.printMobMissingKeyOrLockpick(npc);
      return false;
      }
    else if(keyInst!=size_t(-1)) { // key-only
      sc.printMobMissingKey(npc);
      return false;
      }
    else if(needToPicklock) { // lockpick only
      sc.printMobMissingLockpick(npc);
      return false;
      }

    if(!conditionFunc.empty()) {
      const int check = sc.invokeCond(npc,conditionFunc.c_str());
      if(check==0) {
        // FIXME: proper message
        sc.printNothingToGet();
        return false;
        }
      }

    if(!useWithItem.empty()) {
      size_t it = world.getSymbolIndex(useWithItem.c_str());
      if(it!=size_t(-1) && npc.hasItem(it)==0) {
        sc.printMobMissingItem(npc);
        return false;
        }
      }
    }
  return true;
  }

const Interactive::Pos *Interactive::findFreePos() const {
  for(auto& i:attPos)
    if(i.user==nullptr && i.isAttachPoint()) {
      return &i;
      }
  return nullptr;
  }

Interactive::Pos *Interactive::findFreePos() {
  for(auto& i:attPos)
    if(i.user==nullptr && i.isAttachPoint()) {
      return &i;
      }
  return nullptr;
  }

Tempest::Vec3 Interactive::worldPos(const Interactive::Pos &to) const {
  auto mesh = visual.protoMesh();
  if(mesh==nullptr)
    return Tempest::Vec3();

  auto mat  = transform();
  auto pos  = mesh->mapToRoot(to.node);
  mat.mul(pos);

  Tempest::Vec3 ret = {};
  mat.project(ret);
  return ret;
  }

bool Interactive::isAvailable() const {
  for(auto& i:attPos)
    if(i.user!=nullptr)
      return false;
  return findFreePos()!=nullptr;
  }

bool Interactive::isStaticState() const {
  for(auto& i:attPos)
    if(i.user!=nullptr) {
      if(i.attachMode)
        return loopState;
      return false;
      }
  return loopState;
  }

bool Interactive::isDetachState(const Npc& npc) const {
  for(auto& i:attPos)
    if(i.user==&npc)
      return !(i.attachMode ^ reverseState);
  return !reverseState;
  }

bool Interactive::canQuitAtLastState() const {
  // NOTE: workaround for B_MoveMob.d
  const bool isDoor = (std::strcmp(schemeName(),"DOOR")==0);
  return (vobType==ZenLoad::zCVobData::VT_oCMobDoor && isDoor) ||
         (vobType==ZenLoad::zCVobData::VT_oCMobSwitch)         ||
         reverseState;
  }

bool Interactive::attach(Npc &npc, Interactive::Pos &to) {
  assert(to.user==nullptr);

  auto mat = nodeTranform(npc,to);
  float x=0, y=0, z=0;
  mat.project(x,y,z);

  Tempest::Vec3 mv = {x,y-npc.translateY(),z};
  if(!npc.testMove(mv)) {
    // FIXME: switches on stone-arks
    // return false;
    }

  if((npc.position()-mv).quadLength()>MAX_AI_USE_DISTANCE*MAX_AI_USE_DISTANCE) {
    if(npc.isPlayer()) {
      auto& sc = npc.world().script();
      sc.printMobTooFar(npc);
      }
    if(npc.isPlayer())
      return false; // TODO: same for npc
    }

  if(!checkUseConditions(npc))
    return false;

  if(!useWithItem.empty()) {
    size_t it = world.getSymbolIndex(useWithItem.c_str());
    if(it!=size_t(-1) && npc.hasItem(it)>0) {
      npc.delItem(it,1);
      }
    npc.setCurrentItem(it);
    }

  setPos(npc,mv);
  setDir(npc,mat);

  if(state>0) {
    reverseState = (state>0);
    } else {
    reverseState = false;
    state = 0;
    }

  to.user       = &npc;
  to.attachMode = true;
  to.started    = false;
  return true;
  }

bool Interactive::attach(Npc &npc) {
  for(auto& i:attPos)
    if(i.user==&npc)
      return true;

  if(!isAvailable()) {
    if(npc.isPlayer())
      world.script().printMobAnotherIsUsing(npc);
    return false;
    }

  auto p = findNearest(npc);
  if(p!=nullptr)
    return attach(npc,*p);

  if(npc.isPlayer() && attPos.size()>0)
    world.script().printMobAnotherIsUsing(npc);
  return false;
  }

bool Interactive::dettach(Npc &npc, bool quick) {
  for(auto& i:attPos) {
    if(i.user==&npc) {
      if(state==stateNum && canQuitAtLastState()/* && !npc.isPlayer()*/) {
        i.user       = nullptr;
        i.attachMode = false;
        npc.quitIneraction();
        return true;
        }
      else if(quick) {
        i.user       = nullptr;
        i.attachMode = false;
        if(reverseState)
          state = stateNum; else
          state = 0;
        npc.quitIneraction();
        }
      else {
        i.attachMode = false;
        return true;
        }
      }
    }
  return true;
  }

bool Interactive::isAttached(const Npc& to) {
  for(auto& i:attPos)
    if(i.user==&to)
      return true;
  return false;
  }

void Interactive::setPos(Npc &npc,const Tempest::Vec3& pos) {
  npc.setPosition(pos);
  }

void Interactive::setDir(Npc &npc, const Tempest::Matrix4x4 &mat) {
  float x0=0,y0=0,z0=0;
  float x1=0,y1=0,z1=1;

  mat.project(x0,y0,z0);
  mat.project(x1,y1,z1);

  npc.setDirection(x1-x0,y1-y0,z1-z0);
  }

float Interactive::qDistanceTo(const Npc &npc, const Interactive::Pos &to) const {
  auto p = worldPos(to);
  return npc.qDistTo(p.x,p.y-npc.translateY(),p.z);
  }

Tempest::Matrix4x4 Interactive::nodeTranform(const Npc &npc, const Pos& p) const {
  auto mesh = visual.protoMesh();
  if(mesh==nullptr)
    return Tempest::Matrix4x4();

  Tempest::Matrix4x4 npos;
  npos.identity();
  if(mesh->skeleton!=nullptr) {
    auto id   = mesh->skeleton->findNode(p.name);
    if(id!=size_t(-1))
      npos = visual.bone(id);
    }

  if(p.isDistPos()) {
    float nodeX = npos.at(3,0);
    float nodeY = npos.at(3,1);
    float nodeZ = npos.at(3,2);
    float dist  = std::sqrt(nodeX*nodeX + nodeZ*nodeZ);

    float npcX  = npc.position().x - position().x;
    float npcZ  = npc.position().z - position().z;
    float npcA  = 180.f*std::atan2(npcZ,npcX)/float(M_PI);

    npos.identity();
    npos.rotateOY(-npcA);
    npos.translate(dist,nodeY,0);
    npos.rotateOY(-90);

    float x = position().x+npos.at(3,0);
    float y = position().y+npos.at(3,1);
    float z = position().z+npos.at(3,2);
    npos.set(3,0,x);
    npos.set(3,1,y);
    npos.set(3,2,z);
    return npos;
    }

  auto mat = transform();
  mat.mul(npos);
  return mat;
  }

Tempest::Vec3 Interactive::nodePosition(const Npc &npc, const Pos &p) const {
  auto  mat = nodeTranform(npc,p);
  float x   = mat.at(3,0);
  float y   = mat.at(3,1);
  float z   = mat.at(3,2);
  return {x,y,z};
  }

Tempest::Matrix4x4 Interactive::nodeTranform(const char* nodeName) const {
  auto mesh = visual.protoMesh();
  if(mesh==nullptr || mesh->skeleton==nullptr)
    return Tempest::Matrix4x4();

  auto id  = mesh->skeleton->findNode(nodeName);
  auto ret = transform();
  if(id!=size_t(-1))
    ret.mul(visual.bone(id));
  return ret;
  }

const Animation::Sequence* Interactive::setAnim(Interactive::Anim t) {
  int  st[]     = {state,state+(reverseState ? -t : t)};
  char ss[2][12] = {};

  st[1] = std::max(0,std::min(st[1],stateNum));

  char buf[256]={};
  for(int i=0;i<2;++i){
    if(st[i]<0)
      std::snprintf(ss[i],sizeof(ss[i]),"S0"); else
      std::snprintf(ss[i],sizeof(ss[i]),"S%d",st[i]);
    }

  if(st[0]<0 || st[1]<0)
    std::snprintf(buf,sizeof(buf),"S_S0"); else
  if(st[0]==st[1])
    std::snprintf(buf,sizeof(buf),"S_%s",ss[0]); else
    std::snprintf(buf,sizeof(buf),"T_%s_2_%s",ss[0],ss[1]);

  return visual.startAnimAndGet(buf,world.tickCount());
  }

bool Interactive::setAnim(Npc* npc,Anim dir) {
  Npc::Anim                  dest = dir==Anim::Out ? Npc::Anim::InteractOut : Npc::Anim::InteractIn;
  const Animation::Sequence* sqNpc=nullptr;
  const Animation::Sequence* sqMob=nullptr;
  if(npc!=nullptr) {
    sqNpc = npc->setAnimAngGet(dest,false);
    if(sqNpc==nullptr && dir!=Anim::Out)
      return false;
    }
  sqMob = setAnim(dir);
  if(sqMob==nullptr && sqNpc==nullptr && dir!=Anim::Out)
    return false;

  uint64_t t0 = sqNpc==nullptr ? 0 : uint64_t(sqNpc->totalTime());
  uint64_t t1 = sqMob==nullptr ? 0 : uint64_t(sqMob->totalTime());
  if(dir!=Anim::Active)
    waitAnim = world.tickCount()+std::max(t0,t1);
  return true;
  }

void Interactive::setState(int st) {
  state = st;
  onStateChanged();
  }

const Animation::Sequence* Interactive::animNpc(const AnimationSolver &solver, Anim t) {
  const char* tag      = schemeName();
  int         st[]     = {state,state+(reverseState ? -t : t)};
  char        ss[2][12] = {};
  const char* point    = "";

  if(t==Anim::FromStand) {
    st[0] = -1;
    st[1] = 0;
    }
  else if(t==Anim::ToStand) {
    st[0] = 0;
    st[1] = -1;
    }

  for(auto& i:attPos)
    if(i.user!=nullptr) {
      point = i.posTag();
      }

  st[1] = std::max(-1,std::min(st[1],stateNum));

  char buf[256]={};
  for(int i=0;i<2;++i){
    if(st[i]<0)
      std::snprintf(ss[i],sizeof(ss[i]),"STAND"); else
      std::snprintf(ss[i],sizeof(ss[i]),"S%d",st[i]);
    }

  for(auto pt:{point,""}) {
    if(st[0]==st[1])
      std::snprintf(buf,sizeof(buf),"S_%s%s_%s",tag,pt,ss[0]); else
      std::snprintf(buf,sizeof(buf),"T_%s%s_%s_2_%s",tag,pt,ss[0],ss[1]);
    if(auto ret = solver.solveFrm(buf))
      return ret;
    }
  return nullptr;
  }

void Interactive::marchInteractives(DbgPainter &p) const {
  p.setBrush(Tempest::Color(1.0,0,0,1));

  for(auto& m:attPos){
    auto pos = worldPos(m);

    float x = pos.x;
    float y = pos.y;
    float z = pos.z;
    p.mvp.project(x,y,z);

    x = (0.5f*x+0.5f)*float(p.w);
    y = (0.5f*y+0.5f)*float(p.h);

    p.painter.drawRect(int(x),int(y),1,1);
    p.drawText(int(x), int(y), schemeName());
    }
  }

void Interactive::moveEvent() {
  Vob::moveEvent();
  visual.setObjMatrix(transform());
  }

const char *Interactive::Pos::posTag() const {
  if(name.rfind("_FRONT")==name.size()-6)
    return "_FRONT";
  if(name.rfind("_BACK")==name.size()-5)
    return "_BACK";
  return "";
  }

bool Interactive::Pos::isAttachPoint() const {
  /*
    ZS_POS0, ZS_POS0_FRONT, ZS_POS0_BACK, ZS_POS0_DIST
    ZS_POS1, ZS_POS1_FRONT, ZS_POS1_BACK,
    ZS_POS2, ZS_POS3, ...
  */
  return name.find("ZS_POS")==0;
  }

bool Interactive::Pos::isDistPos() const {
  return name.rfind("_DIST")==name.size()-5;
  }
