// ============================================================
// iKunMorse — Morse trainer for M5StickC Plus
// ============================================================
// 基于 Koch 方法的摩斯密码训练器
// 吉祥物：鸡哥 (ikun_performer 精灵表 8 帧动作)
// 文风：Duolingo 式贱萌吐槽
//
// 训练流程（参考 Morse Mania 的渐进教学）：
//   RECEIVE：新字符先明牌示范，再用 A/B 二选一进行听音辨认
//   TAP：    显示字符 → 用户拍发 → 对照反馈
//
// 从最简单的 E(.)、T(-) 开始；每关答满 6 题且正确率达 80% 自动进入下一关。
//
// 操作：
//   RECEIVE: A/B=选择屏幕对应字母  长按A=提示并重播
//   TAP:     A短按(<300ms)=点  A长按(≥300ms)=划
//            松开0.8秒自动提交  B=清除/跳过  长按B=切回LISTEN
//
// 鸡哥动作：
//   IDLE待机 / DRIBBLE运球 / IRON_SHOULDER铁山靠(答对)
//   SINGING唱歌(连对) / CHICKEN_DANCE鸡舞(新字母)
//   SLEEPY睡觉(连错) / RAPPING说唱(变速) / CROSSOVER变向(切模式)
// ============================================================

#include <M5Unified.h>
#include <Preferences.h>
#include "ikun_sprites.h"
#include "iKunM5Mirror.h"

// ================================================================
// 摩斯码表 — Koch 推荐顺序
// ================================================================
static const char KOCH_ORDER[] = "ETIANMSURWDKGOHVFLPJBXCYZQ";
static constexpr size_t KOCH_TOTAL = sizeof(KOCH_ORDER) - 1;

static const char* const MORSE_CODE[] = {
  ".", "-", "..", ".-", "-.", "--", "...", "..-", ".-.", ".--",
  "-..", "-.-", "--.", "---", "....", "...-", "..-.", ".-..", ".--.",
  ".---", "-...", "-..-", "-.-.", "-.--", "--..", "--.-"
};

// ================================================================
// 鸡哥精灵帧索引
// ================================================================
enum IkunFrame {
  IKUN_IDLE           = 0,
  IKUN_IRON_SHOULDER  = 1,  // 铁山靠 — 答对一击
  IKUN_DRIBBLE        = 2,  // 运球 — 训练中
  IKUN_CROSSOVER      = 3,  // 变向 — 切模式
  IKUN_SINGING        = 4,  // 唱歌 — 连对庆祝
  IKUN_RAPPING        = 5,  // RAP — 变速
  IKUN_CHICKEN_DANCE  = 6,  // 鸡舞 — 新字母
  IKUN_SLEEPY         = 7,  // 睡觉 — 连错
};

// 鸡哥绘制位置（右侧）
static constexpr int IKUN_X = 170;
static constexpr int IKUN_Y = 32;

// ================================================================
// 训练状态
// ================================================================
enum Phase { PHASE_LISTEN, PHASE_TAP, PHASE_FREE };
static Phase   currentPhase    = PHASE_LISTEN;
static size_t  activeCount     = 2;
static int     currentLetter   = 0;
// 20 WPM 保持字符整体节奏，避免养成逐个数点划的习惯。
static int     wpm             = 20;
static constexpr int WPM_MIN   = 5;
static constexpr int WPM_MAX   = 30;

// 拍发输入
enum TapState { TAP_IDLE, TAP_PRESSING, TAP_WAIT };
static TapState  tapState      = TAP_IDLE;
static uint32_t  tapPressStart = 0;
static uint32_t  tapReleaseMs  = 0;
static String    tapSeq        = "";

// 自由输入：不计分，只把点划实时译成字母，适合随手拼着玩。
static String    freeSeq       = "";
static String    freeText      = "";
static char      freeLastLetter = '-';

// 统计
static int   correctCount = 0;
static int   totalCount   = 0;
static int   streak       = 0;
static int   streakFail   = 0;

// 一关的「掌握格」：答对推进，答错倒退；填满才进入新字符教学。
static constexpr int LEVEL_MASTERY_GOAL = 6;
static int   levelMastery = 0;
static int   levelAttempts = 0;
static bool  pendingAutoUnlock = false;
// 首关 E/T 共同计分；之后每关只由新字母填满 6 格。
static int   levelTargetLetter = -1;
static uint8_t questionsSinceTarget = 0;
// 每个字母的遗忘权重；答错后短期内更常出现，答对会逐步消退。
static uint8_t letterMissWeight[KOCH_TOTAL] = {};

// 计时
static uint32_t lastDraw   = 0;
static uint32_t autoNextMs = 0;
static bool     answerShown = false;

// 接收训练：屏幕只需两个答案键，也能覆盖所有已学字符
static int      choiceA = 0;
static int      choiceB = 1;
static int      receiveChoice = -1;
static bool     receiveReady = false;
static bool     receiveFeedbackShown = false;
static uint32_t receiveFeedbackUntil = 0;
// 题目切换后先清空上一题的按键事件，避免教学确认键直接被当作答案。
static bool     receiveInputArmed = false;
static uint32_t receiveOpenedMs = 0;
// 提示由用户主动开启；教学页结束后，答题页默认不显示点划。
static bool     receiveHintVisible = false;
static int      forcedNextLetter = -1;

// 答题反馈：先固定展示本题结果，再进入下一题，避免反馈与题目错位
static bool     feedbackShown  = false;
static bool     lastAnswerOK   = false;
static String   submittedSeq   = "";
static uint32_t feedbackUntil  = 0;

// A+B 组合键，避免把解锁误触成单键操作
static bool     chordActive    = false;
static bool     chordHandled   = false;
static uint32_t chordStartMs   = 0;

// 反馈弹幕
static String   popupMsg     = "";
static uint32_t popupUntil   = 0;
static bool     popupIsGood  = true;

// 鸡哥当前帧
static IkunFrame ikunFrame   = IKUN_IDLE;
static uint32_t  ikunLockUntil = 0;  // 表情锁定到期时间

// 新字母展示
static bool     showingNew   = false;
static int      lessonNextLetter = -1;
// 关卡结算的最后一次按键可能在事件队列里多停留一帧；教学页先等按键完全释放，
// 再接受「开始」输入，避免它被直接跳过并误判为下一题答案。
static bool     lessonInputArmed = false;
static uint32_t lessonOpenedMs = 0;
static uint8_t  lessonPressedButton = 0;  // 1=RIGHT(A), 2=UP(B)

// ================================================================
// 显示
// ================================================================
static M5Canvas canvas(&M5.Display);
static iKunM5Mirror mirror;
static Preferences progressStore;
static bool progressStoreReady = false;
static uint8_t savedActiveCount = 0;
static uint8_t savedMastery = 255;
static constexpr int SCR_W = 240;
static constexpr int SCR_H = 135;

static constexpr uint16_t BG        = 0x0862;
static constexpr uint16_t C_WHITE   = 0xFFFF;
static constexpr uint16_t C_CYAN    = 0xA7FF;
static constexpr uint16_t C_GREEN   = 0x07E0;
static constexpr uint16_t C_RED     = 0xF800;
static constexpr uint16_t C_YELLOW  = 0xFFE0;
static constexpr uint16_t C_ORANGE  = 0xFD20;
static constexpr uint16_t C_DIM     = 0xC618;
static constexpr uint16_t C_DARK    = 0x2104;

// ================================================================
// 贱萌吐槽文案池
// ================================================================
static const char* quipCorrect[] = {
  "NICE!", "GOOD COPY!", "SOLID!", "NAILED IT!",
  "KEEP GOING!", "CLEAN SEND!", "GREAT EAR!", "YES!",
};
static constexpr int QCN = sizeof(quipCorrect) / sizeof(quipCorrect[0]);

static const char* quipWrong[] = {
  "CLOSE - HEAR IT", "TRY THE RHYTHM", "REPLAY & RETRY",
  "ALMOST!", "RESET, YOU GOT THIS", "ONE MORE TIME",
  "LISTEN TO THE GAP", "STAY RELAXED", "BUILD THE SOUND", "KEEP AT IT",
};
static constexpr int QWN = sizeof(quipWrong) / sizeof(quipWrong[0]);

static const char* quipStreak3[] = {
  "3 STREAK!", "RHYTHM LOCKED", "YOU'RE WARM!", "KEEP FLOWING",
};
static constexpr int QS3N = sizeof(quipStreak3) / sizeof(quipStreak3[0]);

static const char* quipStreak5[] = {
  "5 STREAK!", "ON FIRE!", "RADIO READY", "GREAT FOCUS",
};
static constexpr int QS5N = sizeof(quipStreak5) / sizeof(quipStreak5[0]);

static const char* quipStreak10[] = {
  "10 STREAK!", "OUTSTANDING!", "MASTER MODE", "SUPER CLEAN",
};
static constexpr int QS10N = sizeof(quipStreak10) / sizeof(quipStreak10[0]);

static const char* quipFail3[] = {
  "TRY LISTEN MODE", "SLOW DOWN", "HEAR, THEN TAP", "TAKE A BREATH",
};
static constexpr int QF3N = sizeof(quipFail3) / sizeof(quipFail3[0]);

static const char* quipFail5[] = {
  "BACK TO LISTEN", "LOWER THE WPM", "FOCUS ON RHYTHM", "RESET & REBUILD",
};
static constexpr int QF5N = sizeof(quipFail5) / sizeof(quipFail5[0]);

static const char* quipNewLetter[] = {
  "NEW SOUND!", "LEVEL UP!", "LETTER UNLOCKED", "READY FOR MORE",
};
static constexpr int QNLN = sizeof(quipNewLetter) / sizeof(quipNewLetter[0]);

static const char* quipWpmUp[] = {
  "SPEED UP!", "FASTER FLOW", "TEMPO +5", "PUSH THE PACE",
};
static constexpr int QWUN = sizeof(quipWpmUp) / sizeof(quipWpmUp[0]);

static const char* quipWpmDown[] = {
  "SLOW & CLEAR", "TEMPO -5", "BUILD ACCURACY", "STEADY MODE",
};
static constexpr int QWDN = sizeof(quipWpmDown) / sizeof(quipWpmDown[0]);

void showPopup(const char* msg, bool good) {
  popupMsg    = msg;
  popupUntil  = millis() + 1800;
  popupIsGood = good;
}

void showPopup(const String& msg, bool good) {
  popupMsg    = msg;
  popupUntil  = millis() + 1800;
  popupIsGood = good;
}

void setIkun(IkunFrame f, uint32_t lockMs = 1200) {
  ikunFrame     = f;
  ikunLockUntil = millis() + lockMs;
}

void updateIkunAuto() {
  if (millis() < ikunLockUntil) return;
  // 根据当前状态自动选帧
  if (currentPhase == PHASE_LISTEN) {
    ikunFrame = IKUN_DRIBBLE;
  } else {
    if (streak >= 5)       ikunFrame = IKUN_SINGING;
    else if (streakFail >= 3) ikunFrame = IKUN_SLEEPY;
    else if (tapState == TAP_IDLE) ikunFrame = IKUN_IDLE;
    else                    ikunFrame = IKUN_DRIBBLE;
  }
}

// ================================================================
// 蜂鸣器
// ================================================================
void speakerOn() {
  if (!M5.Speaker.isEnabled()) M5.Speaker.end();
  M5.Speaker.setVolume(160);
}

// 成功提示使用固定的上扬双音，与 700Hz 的摩斯单音在音高和节奏上明确区分。
void playSuccessChime() {
  M5.Speaker.tone(1568, 35, -1, true);
  delay(42);
  M5.Speaker.tone(2349, 130, -1, true);
  delay(130);
}

int unitMs()   { return 1200 / wpm; }
int charGapMs() { return unitMs() * 2; }

void playElement(bool isDah) {
  int dur = isDah ? unitMs() * 3 : unitMs();
  M5.Speaker.tone(700, dur, -1, true);
  delay(dur);
  delay(unitMs());
}

void playMorse(const char* code) {
  size_t n = strlen(code);
  for (size_t i = 0; i < n; i++) playElement(code[i] == '-');
  if (n > 0) delay(charGapMs());
}

// ================================================================
// 随机选字母
// ================================================================
void pickLetter() {
  int previous = currentLetter;
  // 新字母采用短→中→长的间隔：先隔 1 题旧题，随后隔 2、3 题。
  if (levelTargetLetter >= 0) {
    int targetGap = levelMastery < 2 ? 1 : (levelMastery < 4 ? 2 : 3);
    if (questionsSinceTarget >= targetGap) {
      currentLetter = levelTargetLetter;
      questionsSinceTarget = 0;
      answerShown = false;
      return;
    }
  }

  int totalWeight = 0;
  for (int i = 0; i < activeCount; i++) {
    if (i == levelTargetLetter) continue;
    if (activeCount > 1 && i == previous) continue;
    totalWeight += 1 + letterMissWeight[i];
  }
  if (totalWeight == 0) {
    for (int i = 0; i < activeCount; i++) {
      if (i == levelTargetLetter) continue;
      totalWeight += 1 + letterMissWeight[i];
    }
  }
  int ticket = random(totalWeight);
  for (int i = 0; i < activeCount; i++) {
    if (i == levelTargetLetter) continue;
    if (activeCount > 1 && i == previous) continue;
    ticket -= 1 + letterMissWeight[i];
    if (ticket < 0) {
      currentLetter = i;
      break;
    }
  }
  if (levelTargetLetter >= 0) questionsSinceTarget++;
  answerShown = false;
}

char decodeMorse(const String& sequence) {
  for (int i = 0; i < KOCH_TOTAL; i++) {
    if (sequence == MORSE_CODE[i]) return KOCH_ORDER[i];
  }
  return '?';
}

void saveProgress() {
  if (!progressStoreReady) return;
  const uint8_t letters = static_cast<uint8_t>(activeCount);
  const uint8_t mastery = static_cast<uint8_t>(levelMastery);
  // 只有值变化才写 NVS，旧字母复习不会反复磨损闪存。
  if (letters != savedActiveCount) {
    progressStore.putUChar("letters", letters);
    savedActiveCount = letters;
  }
  if (mastery != savedMastery) {
    progressStore.putUChar("mastery", mastery);
    savedMastery = mastery;
  }
}

void loadProgress() {
  progressStoreReady = progressStore.begin("ikunmorse", false);
  if (!progressStoreReady) return;
  activeCount = constrain((int)progressStore.getUChar("letters", 2), 2, (int)KOCH_TOTAL);
  levelMastery = constrain((int)progressStore.getUChar("mastery", 0), 0, LEVEL_MASTERY_GOAL);
  levelTargetLetter = activeCount > 2 ? activeCount - 1 : -1;
  savedActiveCount = static_cast<uint8_t>(activeCount);
  savedMastery = static_cast<uint8_t>(levelMastery);
}

void enterFreeMode() {
  currentPhase = PHASE_FREE;
  freeSeq = "";
  freeText = "";
  freeLastLetter = '-';
  tapState = TAP_IDLE;
  setIkun(IKUN_RAPPING, 900);
  lastDraw = 0;
}

void exitFreeMode() {
  currentPhase = PHASE_LISTEN;
  freeSeq = "";
  freeText = "";
  freeLastLetter = '-';
  prepareReceiveQuestion();
  setIkun(IKUN_CROSSOVER, 900);
  lastDraw = 0;
}

void commitFreeCharacter() {
  if (freeSeq.length() == 0) return;
  freeLastLetter = decodeMorse(freeSeq);
  freeText += freeLastLetter;
  if (freeText.length() > 14) freeText.remove(0, freeText.length() - 14);
  freeSeq = "";
  tapState = TAP_IDLE;
}

// ================================================================
// 绘制鸡哥精灵（在 canvas 上）
// ================================================================
void drawIkun(IkunFrame f) {
  canvas.drawPng(ikun_sprites[f], ikun_sprite_lengths[f], IKUN_X, IKUN_Y);
}

// ================================================================
// 绘制 — LISTEN 阶段
// ================================================================
void drawListen() {
  canvas.fillSprite(BG);

  // 顶部状态
  canvas.setTextFont(1);
  canvas.setTextColor(C_CYAN, BG);
  canvas.setCursor(5, 3);
  // 常驻信息只服务当前任务：关卡、当前新字符和本关完成度。
  // 已解锁总数只在结算时出现，不占用练习时的注意力。
  if (activeCount == 2) {
    canvas.printf("LV 1  E/T  %d/%d", levelMastery, LEVEL_MASTERY_GOAL);
  } else {
    canvas.printf("LV %d  %c  %d/%d", (int)activeCount - 1,
                  KOCH_ORDER[levelTargetLetter], levelMastery, LEVEL_MASTERY_GOAL);
  }

  // 当前关进度：答对推进、答错倒退，真正填满才过关。
  int barW = 55, barH = 5, barX = SCR_W - barW - 7, barY = 4;
  canvas.drawRect(barX - 1, barY - 1, barW + 2, barH + 2, C_DIM);
  int fill = min(barW, (int)((long)levelMastery * barW / LEVEL_MASTERY_GOAL));
  if (fill > 0) canvas.fillRect(barX, barY, fill, barH, C_GREEN);

  // 分隔线
  canvas.drawFastVLine(160, 0, SCR_H, C_DIM);

  canvas.setTextDatum(middle_center);
  if (receiveFeedbackShown) {
    bool isCorrect = receiveChoice == currentLetter;
    uint16_t resultColor = isCorrect ? C_GREEN : C_RED;
    char answer[2] = { KOCH_ORDER[currentLetter], '\0' };
    // 用整块高对比结果卡占据答题区，远看也能立即分辨对错。
    canvas.fillRoundRect(7, 18, 147, 82, 16, resultColor);
    canvas.setTextFont(4);
    canvas.setTextSize(2);
    canvas.setTextColor(C_DARK, resultColor);
    canvas.drawString(isCorrect ? "OK" : "NO", 80, 50);
    canvas.setTextSize(1);
    canvas.setTextFont(4);
    canvas.setTextColor(C_DARK, resultColor);
    canvas.drawString(answer, 54, 82);
    canvas.drawString(MORSE_CODE[currentLetter], 108, 82);
  } else {
    canvas.setTextFont(2);
    canvas.setTextColor(receiveReady ? C_WHITE : C_CYAN, BG);
    canvas.drawString(receiveReady ? "WHICH SOUND?" : "LISTEN...", 80, 31);

    // 用空间方向而不是 A/B 命名：上方 B = ↑，右侧 A = →。
    // 答案独占一行，避免按键名和字母被读成 "AE" / "BT"。
    canvas.drawRoundRect(7, 43, 70, 59, 8, C_DIM);
    canvas.drawRoundRect(83, 43, 70, 59, 8, C_DIM);
    canvas.drawFastVLine(42, 48, 10, C_CYAN);
    canvas.fillTriangle(42, 44, 36, 51, 48, 51, C_CYAN);
    canvas.drawFastHLine(109, 49, 12, C_CYAN);
    canvas.fillTriangle(127, 49, 119, 43, 119, 55, C_CYAN);
    canvas.setTextFont(4);
    canvas.setTextColor(C_WHITE, BG);
    char bAnswer[2] = { KOCH_ORDER[choiceB], '\0' };
    char aAnswer[2] = { KOCH_ORDER[choiceA], '\0' };
    canvas.drawString(bAnswer, 42, 77);
    canvas.drawString(aAnswer, 118, 77);

    if (receiveHintVisible) {
      canvas.setTextFont(2);
      canvas.setTextColor(C_YELLOW, BG);
      canvas.drawString(MORSE_CODE[choiceB], 42, 94);
      canvas.drawString(MORSE_CODE[choiceA], 118, 94);
    }
  }

  canvas.setTextDatum(top_left);

  // 鸡哥吐槽弹幕
  if (millis() < popupUntil) {
    canvas.setTextFont(1);
    canvas.setTextColor(popupIsGood ? C_GREEN : C_ORANGE, BG);
    canvas.setCursor(5, receiveFeedbackShown ? 105 : 100);
    canvas.print(popupMsg);
  }

  // 底部提示
  canvas.setTextFont(1);
  canvas.setTextColor(C_DIM, BG);
  canvas.drawString("UP/RIGHT ANSWER  HOLD RIGHT HINT", 5, 122);

  // 鸡哥
  drawIkun(ikunFrame);

  canvas.pushSprite(0, 0);
}

void prepareReceiveQuestion() {
  if (forcedNextLetter >= 0) {
    currentLetter = forcedNextLetter;
    forcedNextLetter = -1;
    questionsSinceTarget = 0;
  } else {
    pickLetter();
  }
  int distractor;
  do {
    distractor = random(activeCount);
  } while (distractor == currentLetter);

  if (random(2) == 0) {
    choiceA = currentLetter;
    choiceB = distractor;
  } else {
    choiceA = distractor;
    choiceB = currentLetter;
  }

  receiveChoice = -1;
  receiveFeedbackShown = false;
  receiveHintVisible = false;
  receiveReady = false;
  drawListen();
  playMorse(MORSE_CODE[currentLetter]);
  receiveReady = true;
  receiveInputArmed = false;
  receiveOpenedMs = millis();
  lastDraw = 0;
}

void submitReceive(int selected) {
  if (!receiveReady || receiveFeedbackShown) return;

  receiveChoice = selected;
  bool ok = selected == currentLetter;
  totalCount++;
  levelAttempts++;

  if (ok) {
    correctCount++;
    // 旧字母答对只是在巩固；新关进度只由本关的新字母推进。
    if (levelTargetLetter < 0 || currentLetter == levelTargetLetter) {
      levelMastery = min(LEVEL_MASTERY_GOAL, levelMastery + 1);
    }
    if (letterMissWeight[currentLetter] > 0) letterMissWeight[currentLetter]--;
    streak++;
    streakFail = 0;
    setIkun(IKUN_IRON_SHOULDER, 1100);
    showPopup(streak >= 5 ? "HEARING LOCKED!" : "CORRECT!", true);
    playSuccessChime();
  } else {
    if (levelTargetLetter < 0 || currentLetter == levelTargetLetter) {
      levelMastery = max(0, levelMastery - 1);
    }
    letterMissWeight[currentLetter] = min(8, letterMissWeight[currentLetter] + 3);
    streak = 0;
    streakFail++;
    setIkun(IKUN_SLEEPY, 1400);
    showPopup("HEAR IT AGAIN", false);
    M5.Speaker.tone(300, 140, -1, true);
    delay(220);
    playMorse(MORSE_CODE[currentLetter]);
  }

  if (activeCount < KOCH_TOTAL && levelMastery >= LEVEL_MASTERY_GOAL) {
    pendingAutoUnlock = true;
    showPopup(String("CLEAR: ") + activeCount + " LETTERS READY", true);
  }

  saveProgress();

  receiveFeedbackShown = true;
  receiveFeedbackUntil = millis() + 1600;
  drawListen();
}

// ================================================================
// 绘制 — TAP 阶段
// ================================================================
void drawTap() {
  canvas.fillSprite(BG);

  int acc = totalCount > 0 ? (int)((long)correctCount * 100 / totalCount) : 0;

  // 顶部
  canvas.setTextFont(1);
  canvas.setTextColor(C_CYAN, BG);
  canvas.setCursor(5, 3);
  canvas.printf("TAP  %dWPM  ACC:%d%%  %d/%d",
                wpm, acc, (int)activeCount, (int)KOCH_TOTAL);

  canvas.drawFastVLine(160, 0, SCR_H, C_DIM);

  // 目标字母（大字）
  canvas.setTextFont(7);
  canvas.setTextColor(C_WHITE, BG);
  canvas.setTextDatum(middle_center);
  char letter[2] = { KOCH_ORDER[currentLetter], '\0' };
  canvas.drawString(letter, 88, 42);
  canvas.setTextDatum(top_left);

  // 练习时隐藏答案；提交后同时展示发送内容和正确答案
  canvas.setTextFont(2);
  if (feedbackShown) {
    canvas.setTextColor(lastAnswerOK ? C_GREEN : C_RED, BG);
    canvas.setCursor(5, 68);
    canvas.printf("YOU: %s", submittedSeq.c_str());
    canvas.setTextColor(C_YELLOW, BG);
    canvas.setCursor(5, 86);
    canvas.printf("KEY: %s", MORSE_CODE[currentLetter]);
  } else if (tapSeq.length() > 0) {
    canvas.setTextColor(C_YELLOW, BG);
    canvas.setCursor(5, 76);
    canvas.printf("YOU: %s", tapSeq.c_str());
  } else {
    canvas.setTextColor(C_DIM, BG);
    canvas.setCursor(5, 76);
    canvas.print("TAP FROM MEMORY");
  }

  // 吐槽
  if (millis() < popupUntil) {
    canvas.setTextFont(1);
    canvas.setTextColor(popupIsGood ? C_GREEN : C_ORANGE, BG);
    canvas.setCursor(5, 104);
    canvas.print(popupMsg);
  }

  // 底部
  canvas.setTextFont(1);
  canvas.setTextColor(C_DIM, BG);
  canvas.drawString("A:DOT/DAH  B:CLEAR/SKIP  HOLD B:BACK", 5, 122);

  // 鸡哥
  drawIkun(ikunFrame);

  canvas.pushSprite(0, 0);
}

// ================================================================
// 绘制 — 自由输入
// ================================================================
void drawFree() {
  canvas.fillSprite(BG);
  canvas.setTextDatum(top_left);
  canvas.setTextFont(1);
  canvas.setTextColor(C_CYAN, BG);
  canvas.drawString("FREE INPUT", 5, 4);
  canvas.setTextColor(C_DIM, BG);
  canvas.drawRightString("HOLD UP: GAME", 154, 4);
  canvas.drawFastVLine(160, 0, SCR_H, C_DIM);

  canvas.setTextDatum(middle_center);
  canvas.setTextFont(2);
  canvas.setTextColor(C_YELLOW, BG);
  // 未输入时留白，不放无意义的占位文案；点划一松手便在这里出现。
  if (freeSeq.length()) canvas.drawString(freeSeq, 80, 28);

  // 最后识别的字母始终大字展示；输入中的点划不会提前泄露答案。
  canvas.setTextFont(4);
  canvas.setTextSize(4);
  canvas.setTextColor(C_WHITE, BG);
  char letter[2] = { freeLastLetter, '\0' };
  canvas.drawString(letter, 80, 70);
  canvas.setTextSize(1);

  canvas.setTextFont(2);
  canvas.setTextColor(C_GREEN, BG);
  canvas.drawString(freeText.length() ? freeText : "_", 80, 105);
  canvas.setTextDatum(top_left);

  canvas.setTextFont(1);
  canvas.setTextColor(C_DIM, BG);
  canvas.drawString("A DOT/DAH  B DELETE  HOLD B GAME", 5, 122);
  drawIkun(IKUN_RAPPING);
  canvas.pushSprite(0, 0);
}

// ================================================================
// 绘制 — 新字母展示
// ================================================================
void drawNewLetterScreen() {
  canvas.fillSprite(BG);

  canvas.setTextDatum(middle_center);
  canvas.setTextFont(2);
  canvas.setTextColor(C_YELLOW, BG);
  canvas.drawString("NEW LETTER!", 90, 18);

  // Font 7 在部分固件上是数字字体；用放大的 Font 4 保证字母一定可见。
  canvas.setTextFont(4);
  canvas.setTextSize(2);
  canvas.setTextColor(C_WHITE, BG);
  char letter[2] = { KOCH_ORDER[currentLetter], '\0' };
  canvas.drawString(letter, 90, 58);
  canvas.setTextSize(1);

  canvas.setTextFont(4);
  canvas.setTextColor(C_YELLOW, BG);
  canvas.drawString(MORSE_CODE[currentLetter], 90, 95);
  canvas.setTextDatum(top_left);

  // 鸡哥跳舞
  drawIkun(IKUN_CHICKEN_DANCE);

  canvas.setTextFont(1);
  canvas.setTextColor(C_DIM, BG);
  canvas.drawString("RIGHT: START   UP: REPLAY", 5, 122);

  canvas.pushSprite(0, 0);
}

void startLesson(int letter, int nextLetter = -1) {
  currentLetter = letter;
  lessonNextLetter = nextLetter;
  showingNew = true;
  receiveReady = false;
  receiveFeedbackShown = false;
  receiveChoice = -1;
  receiveHintVisible = false;
  receiveInputArmed = false;
  lessonInputArmed = false;
  lessonOpenedMs = millis();
  lessonPressedButton = 0;
  drawNewLetterScreen();
  speakerOn();
  // 一次只示范一个完整字符，重播完全由用户控制。
  playMorse(MORSE_CODE[currentLetter]);
}

void finishLesson() {
  if (lessonNextLetter >= 0) {
    int nextLetter = lessonNextLetter;
    startLesson(nextLetter);
    return;
  }

  showingNew = false;
  autoNextMs = millis();
  lastDraw = 0;
  prepareReceiveQuestion();
}

// ================================================================
// 解锁新字母
// ================================================================
void addNewLetter() {
  if (activeCount >= KOCH_TOTAL) return;

  activeCount++;
  currentPhase  = PHASE_LISTEN;
  currentLetter = activeCount - 1;
  answerShown   = false;
  tapSeq        = "";
  tapState      = TAP_IDLE;
  feedbackShown = false;
  submittedSeq  = "";
  setIkun(IKUN_CHICKEN_DANCE, 4000);

  correctCount = 0;
  totalCount   = 0;
  levelMastery = 0;
  levelAttempts = 0;
  memset(letterMissWeight, 0, sizeof(letterMissWeight));
  levelTargetLetter = currentLetter;
  questionsSinceTarget = 0;
  pendingAutoUnlock = false;
  streak       = 0;
  streakFail   = 0;
  forcedNextLetter = currentLetter;
  autoNextMs   = millis();
  lastDraw     = 0;
  saveProgress();
  startLesson(currentLetter);
}

void requestNewLetter() {
  if (activeCount >= KOCH_TOTAL) {
    showPopup("ALL LETTERS UNLOCKED", true);
    return;
  }

  int accuracy = totalCount > 0
    ? (int)((long)correctCount * 100 / totalCount)
    : 0;
  if (totalCount < 8) {
    showPopup("FINISH 8 TAP TRIALS", false);
    setIkun(IKUN_SLEEPY, 1400);
    return;
  }
  if (accuracy < 80) {
    showPopup("REACH 80% ACCURACY", false);
    setIkun(IKUN_SLEEPY, 1400);
    return;
  }

  addNewLetter();
}

// ================================================================
// TAP 提交
// ================================================================
void submitTap() {
  if (tapSeq.length() == 0) return;

  const char* correct = MORSE_CODE[currentLetter];
  bool ok = (tapSeq == String(correct));

  submittedSeq = tapSeq;
  lastAnswerOK = ok;
  feedbackShown = true;
  tapSeq = "";
  tapState = TAP_IDLE;
  drawTap();

  totalCount++;
  if (ok) {
    correctCount++;
    streak++;
    streakFail = 0;

    // 成功提示音与摩斯码使用不同音高轮廓，避免干扰听音记忆。
    playSuccessChime();

    // 铁山靠！
    setIkun(IKUN_IRON_SHOULDER, 1500);

    if (streak >= 10)      showPopup(quipStreak10[random(QS10N)], true);
    else if (streak >= 5)  showPopup(quipStreak5[random(QS5N)], true);
    else if (streak >= 3)  showPopup(quipStreak3[random(QS3N)], true);
    else                   showPopup(quipCorrect[random(QCN)], true);

  } else {
    streakFail++;
    streak = 0;

    // 错误音
    M5.Speaker.tone(300, 200, -1, true);
    delay(200);
    delay(150);
    playMorse(correct);

    setIkun(IKUN_SLEEPY, 1800);

    if (streakFail >= 5)      showPopup(quipFail5[random(QF5N)], false);
    else if (streakFail >= 3) showPopup(quipFail3[random(QF3N)], false);
    else                      showPopup(quipWrong[random(QWN)], false);
  }

  // 从音效结束后开始计算停留时间，让用户看清“我的答案 / 正确答案”。
  feedbackUntil = millis() + 1800;
}

// ================================================================
// setup
// ================================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  // StickC Plus 的 FTDI 不支持 921600；750000 是官方支持档位。
  Serial.begin(750000);
  mirror.begin(Serial);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(100);

  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCR_W, SCR_H)) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("MEMORY ERROR", SCR_W / 2, SCR_H / 2);
    while (true) delay(1000);
  }

  randomSeed(analogRead(0));
  speakerOn();
  setIkun(IKUN_DRIBBLE);
  loadProgress();

  // 首次使用教学 E/T；有存档时复习当前最新字母后继续原关卡。
  if (activeCount == 2) startLesson(0, 1);
  else                  startLesson(levelTargetLetter);
}

// ================================================================
// loop
// ================================================================
void loop() {
  M5.update();
  mirror.poll();
  if (mirror.consumeEnabledEvent()) {
    // 让设备屏幕也确认浏览器的 ON 是否真的到达，便于排查 USB 单向异常。
    showPopup("USB MIRROR ON", true);
    lastDraw = 0;
  }
  // capture 必须放在 loop 顶层：教学页/反馈页等分支会提前 return，
  // 若只在 drawListen/drawTap 里调用，停留在这些页面时永远不会发帧。
  mirror.capture(canvas);

  bool bothHeld = M5.BtnA.isPressed() && M5.BtnB.isPressed();

  // 教学页停留直到用户决定继续：→ 开始/下一字母，↑ 重播。
  if (showingNew) {
    // 先吞掉来自上一题的点击事件。用户松开所有按键并看过极短的保护时间后，
    // 才把下一次点击当作教学页的操作。
    if (!lessonInputArmed) {
      if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()
          && millis() - lessonOpenedMs >= 180) {
        lessonInputArmed = true;
      }
      return;
    }
    // 不使用 wasClicked：它可能是上一题残留的 release 事件。必须先看到新的按下，
    // 再在该按键松开时执行教学操作。
    if (lessonPressedButton == 0) {
      if (M5.BtnA.isPressed()) lessonPressedButton = 1;
      else if (M5.BtnB.isPressed()) lessonPressedButton = 2;
      return;
    }
    if (lessonPressedButton == 1 && !M5.BtnA.isPressed()) {
      finishLesson();
    } else if (lessonPressedButton == 2 && !M5.BtnB.isPressed()) {
      lessonPressedButton = 0;
      drawNewLetterScreen();
      playMorse(MORSE_CODE[currentLetter]);
    }
    return;
  }

  // A+B 是唯一的解锁手势。组合键期间吞掉单键事件，防止误重播、误切模式。
  if (!chordActive && bothHeld) {
    chordActive = true;
    chordHandled = false;
    chordStartMs = millis();
    tapSeq = "";
    tapState = TAP_IDLE;
  }
  if (chordActive) {
    if (bothHeld && !chordHandled && millis() - chordStartMs >= 1000) {
      chordHandled = true;
      requestNewLetter();
    }
    if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) {
      chordActive = false;
      chordHandled = false;
      lastDraw = 0;
    }
    return;
  }

  // 自动恢复鸡哥帧
  updateIkunAuto();

  // ============================================================
  // LISTEN
  // ============================================================
  if (currentPhase == PHASE_LISTEN) {

    if (receiveFeedbackShown) {
      if (millis() >= receiveFeedbackUntil) {
        popupUntil = 0;
        if (pendingAutoUnlock) {
          pendingAutoUnlock = false;
          addNewLetter();
          // 自动升关已切到教学状态；不能再继续执行本轮 LISTEN 的输入逻辑。
          return;
        } else {
          prepareReceiveQuestion();
        }
      } else {
        if (millis() - lastDraw > 120) {
          lastDraw = millis();
          drawListen();
        }
        return;
      }
    }

    // 教学确认/上一题答案的 click 事件可能跨越一次 M5.update()；在安全窗口内
    // 主动读取并丢弃它们，直到按键都松开后才接收下一题的全新输入。
    if (!receiveInputArmed) {
      M5.BtnA.wasClicked();
      M5.BtnB.wasClicked();
      M5.BtnA.wasReleaseFor(800);
      if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()
          && millis() - receiveOpenedMs >= 180) {
        receiveInputArmed = true;
      }
      return;
    }

    // 长按上键切入自由输入；先判断它，避免松开时被当作左侧答案。
    if (M5.BtnB.wasReleaseFor(800)) {
      enterFreeMode();
      return;
    }

    if (M5.BtnA.wasClicked()) submitReceive(choiceA);
    if (M5.BtnB.wasClicked()) submitReceive(choiceB);

    // 求助会显示两个候选码并重播，不扣分；之后仍由用户作答。
    if (M5.BtnA.wasReleaseFor(800)) {
      receiveHintVisible = true;
      receiveReady = false;
      drawListen();
      playMorse(MORSE_CODE[currentLetter]);
      receiveReady = true;
      showPopup("HINT SHOWN", true);
      lastDraw = 0;
    }

  }

  // ============================================================
  // FREE INPUT
  // ============================================================
  else if (currentPhase == PHASE_FREE) {
    // 长按 B 回到游戏；切换时清空自由输入内容。
    if (M5.BtnB.wasReleaseFor(800)) {
      exitFreeMode();
      return;
    }

    switch (tapState) {
      case TAP_IDLE:
        if (M5.BtnA.wasPressed()) {
          tapPressStart = millis();
          tapState = TAP_PRESSING;
          // 自由输入要让声音和手指同步：按下即起音，松开才停止。
          M5.Speaker.tone(700);
        }
        break;

      case TAP_PRESSING:
        if (M5.BtnA.wasReleased()) {
          uint32_t held = millis() - tapPressStart;
          M5.Speaker.stop();
          freeSeq += (held < 300) ? '.' : '-';
          tapReleaseMs = millis();
          tapState = TAP_WAIT;
          // 先让用户看见刚按出的点/划，再等待字符间隔进行识别。
          drawFree();
        }
        break;

      case TAP_WAIT:
        if (M5.BtnA.wasPressed()) {
          tapPressStart = millis();
          tapState = TAP_PRESSING;
        } else if (millis() - tapReleaseMs > 700) {
          commitFreeCharacter();
          drawFree();
        }
        break;
    }

    if (M5.BtnB.wasClicked()) {
      if (freeSeq.length() > 0) {
        freeSeq.remove(freeSeq.length() - 1);
        tapState = freeSeq.length() ? TAP_WAIT : TAP_IDLE;
      } else if (freeText.length() > 0) {
        freeText.remove(freeText.length() - 1);
        freeLastLetter = freeText.length() ? freeText[freeText.length() - 1] : '-';
      }
      lastDraw = 0;
    }
  }

  // ============================================================
  // TAP
  // ============================================================
  else if (currentPhase == PHASE_TAP) {

    if (feedbackShown) {
      if (millis() >= feedbackUntil) {
        feedbackShown = false;
        submittedSeq = "";
        popupUntil = 0;
        pickLetter();
        lastDraw = 0;
      } else {
        if (millis() - lastDraw > 120) {
          lastDraw = millis();
          drawTap();
        }
        return;
      }
    }

    switch (tapState) {
      case TAP_IDLE:
        if (M5.BtnA.wasPressed() && !bothHeld) {
          tapPressStart = millis();
          tapState = TAP_PRESSING;
        }
        break;

      case TAP_PRESSING:
        if (M5.BtnA.wasReleased()) {
          uint32_t held = millis() - tapPressStart;
          tapSeq += (held < 300) ? '.' : '-';
          M5.Speaker.tone(700, 25, -1, true);
          tapReleaseMs = millis();
          tapState = TAP_WAIT;
        }
        break;

      case TAP_WAIT:
        if (M5.BtnA.wasPressed() && !bothHeld) {
          tapPressStart = millis();
          tapState = TAP_PRESSING;
        } else if (millis() - tapReleaseMs > 800) {
          submitTap();
        }
        break;
    }

    if (M5.BtnB.wasClicked()) {
      if (tapSeq.length() > 0) {
        tapSeq   = "";
        tapState = TAP_IDLE;
        showPopup("INPUT CLEARED", true);
      } else {
        // 空输入时短按 B = 跳过本题，但仍留在拍发练习。
        pickLetter();
        showPopup("SKIPPED", true);
      }
    }

    if (M5.BtnB.wasReleaseFor(800)) {
      tapSeq   = "";
      tapState = TAP_IDLE;
      feedbackShown = false;
      currentPhase = PHASE_LISTEN;
      prepareReceiveQuestion();
      autoNextMs = millis();
      setIkun(IKUN_CROSSOVER, 1000);
      lastDraw = 0;
    }
  }

  // ============================================================
  // 绘制
  // ============================================================
  uint32_t now = millis();
  if (now - lastDraw > 120) {
    lastDraw = now;
    if (currentPhase == PHASE_LISTEN) drawListen();
    else if (currentPhase == PHASE_FREE) drawFree();
    else                              drawTap();
  }
}
