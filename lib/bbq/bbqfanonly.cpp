#include "bbqfanonly.h"

#include <memory>
#include <algorithm>

#ifdef UNIT_TEST
#include <iostream>
#include <iomanip>
#endif
#include <FuzzyRule.h>
#include <FuzzyComposition.h>
#include <Fuzzy.h>
#include <FuzzyRuleConsequent.h>
#include <FuzzyOutput.h>
#include <FuzzyInput.h>
#include <FuzzyIO.h>
#include <FuzzySet.h>
#include <FuzzyRuleAntecedent.h>

#include <cmath>

#define TEMP_ERROR_INPUT 1
#define TEMP_CHANGE_INPUT 2

#define FAN_OUTPUT 1
#define LID_ALERT_OUTPUT 2
#define CHARCOAL_ALERT_OUTPUT 3

#define LID_OPEN_ALERT_RULE 20
#define LID_CLOSE_ALERT_RULE 21
#define CHARCOAL_ALERT_RULE 10

#define TEMP_CHANGE_DELAY 5000

// Measured with my own BBQ temp will drop around 5 degrees in 10 seconds after I open the lid
// Since we get called every 5 seconds this is the temperature difference we need to detect
#define TEMP_DROP_LID_OPEN_FIVE_SEC ((135-126) / 2) - 1)

// Once we detect a temperature rise we assume the lid is closed again
#define TEMP_RISE_LID_CLOSE TEMP_DROP_LID_OPEN_FIVE_SEC

BBQFanOnly::BBQFanOnly(std::shared_ptr<TemperatureSensor> pTempSensor,
                       std::shared_ptr<Ventilator> pFan) :
    BBQ(),
    m_tempSensor(pTempSensor),
    m_fan(pFan),
    m_fuzzy(new Fuzzy()),
    m_setPoint(20.0f),
    m_lidOpenTriggered(false),
    m_periodStartMillis(0) {
    m_tempStore.fill(m_tempSensor->get());
}

BBQFanOnly::~BBQFanOnly() {
    m_fan->speed(0.f);
}

void BBQFanOnly::config(const BBQFanOnlyConfig& p_config) {
    m_config = p_config;
}

BBQFanOnlyConfig BBQFanOnly::config() const {
    return m_config;
}

void BBQFanOnly::init() {
    delete m_fuzzy;
    m_fuzzy = new Fuzzy();

    // Create input for Temperature errors
    FuzzyInput* tempErrorInput = new FuzzyInput(TEMP_ERROR_INPUT);
    m_fuzzy->addFuzzyInput(tempErrorInput);

    FuzzySet* tempErrorNegativeHigh = fuzzyFromVector(m_config.temp_error_hight, true);
    tempErrorInput->addFuzzySet(tempErrorNegativeHigh);
    FuzzySet* tempErrorNegativeMedium = fuzzyFromVector(m_config.temp_error_medium, true);
    tempErrorInput->addFuzzySet(tempErrorNegativeMedium);
    FuzzySet* tempErrorLow = fuzzyFromVector(m_config.temp_error_low, false);
    tempErrorInput->addFuzzySet(tempErrorLow);
    FuzzySet* tempErrorPositiveMedium = fuzzyFromVector(m_config.temp_error_medium, false);
    tempErrorInput->addFuzzySet(tempErrorPositiveMedium);
    FuzzySet* tempErrorPositiveHigh = fuzzyFromVector(m_config.temp_error_hight, false);
    tempErrorInput->addFuzzySet(tempErrorPositiveHigh);

    // Input for temperature changes
    FuzzyInput* tempDrop = new FuzzyInput(TEMP_CHANGE_INPUT);
    m_fuzzy->addFuzzyInput(tempDrop);
    FuzzySet* tempDecreasesFast = fuzzyFromVector(m_config.temp_change_fast, true);
    tempDrop->addFuzzySet(tempDecreasesFast);
    FuzzySet* tempDecreasesMedium = fuzzyFromVector(m_config.temp_change_medium, true);
    tempDrop->addFuzzySet(tempDecreasesMedium);
    FuzzySet* tempChangesSlow = fuzzyFromVector(m_config.temp_change_slow, false);
    tempDrop->addFuzzySet(tempChangesSlow);
    FuzzySet* tempIncreasedMedium = fuzzyFromVector(m_config.temp_change_medium, false);
    tempDrop->addFuzzySet(tempIncreasedMedium);
    FuzzySet* tempIncreasesFast = fuzzyFromVector(m_config.temp_change_fast, false);
    tempDrop->addFuzzySet(tempIncreasesFast);

    // Create Output for Lid Open Detection
    std::array<float, 2> lidDetectOutputArray = { {0, 1} };

    FuzzyOutput* lidOpenDection = new FuzzyOutput(LID_OPEN_ALERT_RULE);
    m_fuzzy->addFuzzyOutput(lidOpenDection);
    FuzzySet* lidOpenDetectOutput = fuzzyFromVector(lidDetectOutputArray, false);
    joinSingle(LID_OPEN_ALERT_RULE, tempDecreasesMedium, lidOpenDetectOutput);

    // Create Output for Lid Close Detection
    FuzzyOutput* lidCloseDection = new FuzzyOutput(LID_CLOSE_ALERT_RULE);
    m_fuzzy->addFuzzyOutput(lidCloseDection);
    FuzzySet* lidCloseDetectOutput = fuzzyFromVector(lidDetectOutputArray, false);
    joinSingle(LID_CLOSE_ALERT_RULE, tempIncreasedMedium, lidCloseDetectOutput);


    // Create Output for Fan
    FuzzyOutput* fan = new FuzzyOutput(FAN_OUTPUT);
    m_fuzzy->addFuzzyOutput(fan);

    //FuzzySet* fanOff = new FuzzySet(0, 0, 0, 0);
    //fan->addFuzzySet(fanOff);
    FuzzySet* fanLower = fuzzyFromVector(m_config.fan_lower, false);
    fan->addFuzzySet(fanLower);
    FuzzySet* fanSteady = fuzzyFromVector(m_config.fan_steady, false);
    fan->addFuzzySet(fanSteady);
    FuzzySet* fanHigher = fuzzyFromVector(m_config.fan_higher, false);
    fan->addFuzzySet(fanHigher);

    uint8_t rule = 30;
    joinSingle(rule++, tempErrorNegativeHigh, fanHigher);
    joinSingle(rule++, tempErrorPositiveHigh, fanLower);

    // 32
    joinSingleAND(rule++, tempErrorNegativeMedium, tempIncreasesFast, fanHigher);
    joinSingleAND(rule++, tempErrorNegativeMedium, tempIncreasedMedium, fanHigher);
    joinSingleAND(rule++, tempErrorNegativeMedium, tempChangesSlow, fanHigher);
    joinSingleAND(rule++, tempErrorNegativeMedium, tempDecreasesMedium, fanHigher);
    joinSingleAND(rule++, tempErrorNegativeMedium, tempDecreasesFast, fanHigher);

    // 37
    joinSingleAND(rule++, tempErrorLow, tempIncreasesFast, fanLower);
    joinSingleAND(rule++, tempErrorLow, tempIncreasedMedium, fanLower);
    joinSingleAND(rule++, tempErrorLow, tempChangesSlow, fanSteady);
    joinSingleAND(rule++, tempErrorLow, tempDecreasesMedium, fanHigher);
    joinSingleAND(rule++, tempErrorLow, tempDecreasesFast, fanHigher);

    // 42
    joinSingleAND(rule++, tempErrorPositiveMedium, tempIncreasesFast, fanLower);
    joinSingleAND(rule++, tempErrorPositiveMedium, tempIncreasedMedium, fanLower);
    joinSingleAND(rule++, tempErrorPositiveMedium, tempChangesSlow, fanLower);
    joinSingleAND(rule++, tempErrorPositiveMedium, tempDecreasesMedium, fanLower);
    joinSingleAND(rule++, tempErrorPositiveMedium, tempDecreasesFast, fanLower);
}


void BBQFanOnly::setPoint(float setTemp) {
    m_setPoint = setTemp;
}
float BBQFanOnly::setPoint() const {
    return m_setPoint;
}

void BBQFanOnly::handle(const uint32_t millis) {
    if (millis - m_periodStartMillis < 1000 / UPDATES_PER_SECOND) {
        return;
    }

    m_periodStartMillis = millis;

    // rotate right and store on the first position the latest temperature
    std::rotate(m_tempStore.begin(), m_tempStore.begin() + m_tempStore.size() - 1, m_tempStore.end());
    m_tempStore[0] = m_tempSensor->get();

    // Feed temp error
    m_fuzzy->setInput(TEMP_ERROR_INPUT, lastErrorInput());

    // Temperature change input
    m_fuzzy->setInput(TEMP_CHANGE_INPUT, tempChangeInput());

    /////////////////////////////////////////////

    // Run fuzzy rules
    m_fuzzy->fuzzify();

    // If the temperature drops more than TEMP_DROP_LID_OPEN_FIVE_SEC
    // We just do it by detecting if the rule is fired
    m_lidOpenTriggered = (m_lidOpenTriggered || m_fuzzy->isFiredRule(LID_OPEN_ALERT_RULE)) && !m_fuzzy->isFiredRule(LID_CLOSE_ALERT_RULE);

    // We choice to control speed instead speed override because we always want a user to override
    // the speed of the fan themselve
    // When lid is detected as open we keep current fan speed

    if (m_lidOpenTriggered && false) {
        m_fan->speed(m_config.fan_speed_lid_open);
    } else {
        m_fan->increase(m_fuzzy->defuzzify(FAN_OUTPUT));
    }

}

bool BBQFanOnly::ruleFired(uint8_t i) {
    return m_fuzzy->isFiredRule(i);
}

bool BBQFanOnly::lowCharcoal() {
    return m_fuzzy->isFiredRule(CHARCOAL_ALERT_RULE);
}

bool BBQFanOnly::lidOpen() {
    return m_lidOpenTriggered;
}

FuzzyRule* BBQFanOnly::joinSingle(int rule, FuzzySet* fi, FuzzySet* fo) {
    FuzzyRuleAntecedent* ifCondition = new FuzzyRuleAntecedent();
    ifCondition->joinSingle(fi);
    FuzzyRuleConsequent* thenConsequence = new FuzzyRuleConsequent();
    thenConsequence->addOutput(fo);
    FuzzyRule* fr = new FuzzyRule(rule, ifCondition, thenConsequence);
    m_fuzzy->addFuzzyRule(fr);
    return fr;
}

FuzzyRule* BBQFanOnly::joinSingleAND(int rule, FuzzySet* fi1, FuzzySet* fi2, FuzzySet* fo) {
    FuzzyRuleAntecedent* ifCondition = new FuzzyRuleAntecedent();
    ifCondition->joinWithAND(fi1, fi2);
    FuzzyRuleConsequent* thenConsequence = new FuzzyRuleConsequent();
    thenConsequence->addOutput(fo);
    FuzzyRule* fr = new FuzzyRule(rule, ifCondition, thenConsequence);
    m_fuzzy->addFuzzyRule(fr);
    return fr;
}
