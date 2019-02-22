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
#define FAN_INPUT 3

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
    m_setPoint(m_tempSensor->get()),
    m_tempLastError(0.0),
    m_fanCurrentSpeed(m_fan->speed()),
    m_tempLast(m_tempSensor->get()),
    m_lastTempChange(0.0),
    m_lidOpenTriggered(false) {
}

BBQFanOnly::~BBQFanOnly() {
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
    FuzzySet* tdf = fuzzyFromVector(m_config.temp_change_fast, true);
    tempDrop->addFuzzySet(tdf);
    FuzzySet* tdm = fuzzyFromVector(m_config.temp_change_medium, true);
    tempDrop->addFuzzySet(tdm);
    FuzzySet* tds = fuzzyFromVector(m_config.temp_change_slow, false);
    tempDrop->addFuzzySet(tds);
    FuzzySet* thm = fuzzyFromVector(m_config.temp_change_medium, false);
    tempDrop->addFuzzySet(thm);
    FuzzySet* thf = fuzzyFromVector(m_config.temp_change_fast, false);
    tempDrop->addFuzzySet(thf);

    // Create Output for Lid Open Detection
    std::array<float, 2> lidDetectOutputArray = { {0, 1} };

    FuzzyOutput* lidOpenDection = new FuzzyOutput(LID_OPEN_ALERT_RULE);
    m_fuzzy->addFuzzyOutput(lidOpenDection);
    FuzzySet* lidOpenDetectOutput = fuzzyFromVector(lidDetectOutputArray, false);
    joinSingle(LID_OPEN_ALERT_RULE, tdf, lidOpenDetectOutput);

    // Create Output for Lid Close Detection
    FuzzyOutput* lidCloseDection = new FuzzyOutput(LID_CLOSE_ALERT_RULE);
    m_fuzzy->addFuzzyOutput(lidCloseDection);
    FuzzySet* lidCloseDetectOutput = fuzzyFromVector(lidDetectOutputArray, false);
    joinSingle(LID_CLOSE_ALERT_RULE, thf, lidCloseDetectOutput);


    // Create Output for Fan
    FuzzyOutput* fan = new FuzzyOutput(FAN_OUTPUT);
    m_fuzzy->addFuzzyOutput(fan);

    FuzzySet* fanOff = new FuzzySet(0, 0, 0, 0);
    fan->addFuzzySet(fanOff);
    FuzzySet* fanLow = fuzzyFromVector(m_config.fan_low, false);
    fan->addFuzzySet(fanLow);
    FuzzySet* fanMedium = fuzzyFromVector(m_config.fan_medium, false);
    fan->addFuzzySet(fanMedium);
    FuzzySet* fanHigh = fuzzyFromVector(m_config.fan_high, false);
    fan->addFuzzySet(fanHigh);

    uint8_t rule = 30;
    joinSingleAND(rule++, tempErrorNegativeHigh, tdf, fanHigh);
    joinSingleAND(rule++, tempErrorNegativeHigh, tdm, fanHigh);
    joinSingleAND(rule++, tempErrorNegativeHigh, tds, fanHigh);
    joinSingleAND(rule++, tempErrorNegativeHigh, thm, fanHigh);
    joinSingleAND(rule++, tempErrorNegativeHigh, thf, fanHigh);

    // 35
    joinSingleAND(rule++, tempErrorNegativeMedium, tdf, fanHigh);
    joinSingleAND(rule++, tempErrorNegativeMedium, tdm, fanHigh);
    joinSingleAND(rule++, tempErrorNegativeMedium, tds, fanHigh);
    joinSingleAND(rule++, tempErrorNegativeMedium, thm, fanMedium);
    joinSingleAND(rule++, tempErrorNegativeMedium, thf, fanMedium);

    // 40
    joinSingleAND(rule++, tempErrorLow, tdf, fanMedium);
    joinSingleAND(rule++, tempErrorLow, tdm, fanMedium);
    joinSingleAND(rule++, tempErrorLow, tds, fanMedium);
    joinSingleAND(rule++, tempErrorLow, thm, fanLow);
    joinSingleAND(rule++, tempErrorLow, thf, fanLow);

    // 45
    joinSingleAND(rule++, tempErrorPositiveMedium, tdf, fanMedium);
    joinSingleAND(rule++, tempErrorPositiveMedium, tdm, fanLow);
    joinSingleAND(rule++, tempErrorPositiveMedium, tds, fanLow);
    joinSingleAND(rule++, tempErrorPositiveMedium, thm, fanOff);
    joinSingleAND(rule++, tempErrorPositiveMedium, thf, fanOff);
}


void BBQFanOnly::setPoint(float setTemp) {
    m_setPoint = setTemp;
}
float BBQFanOnly::setPoint() const {
    return m_setPoint;
}

void BBQFanOnly::handle() {
    m_tempLastError = m_tempSensor->get() - m_setPoint;
    m_lastTempChange = m_tempSensor->get() - m_tempLast;

    // Feed temp error
    m_fuzzy->setInput(TEMP_ERROR_INPUT, m_tempLastError);

    // FAN Input
    m_fuzzy->setInput(FAN_INPUT, m_fanCurrentSpeed);

    // Temperature change input
    m_fuzzy->setInput(TEMP_CHANGE_INPUT, m_lastTempChange);

    /////////////////////////////////////////////

    // Run fuzzy rules
    m_fuzzy->fuzzify();

    // Get fan

    // If the temperature drops more than TEMP_DROP_LID_OPEN_FIVE_SEC
    // We just do it by detecting if the rule is fired
    m_lidOpenTriggered = (m_lidOpenTriggered || m_fuzzy->isFiredRule(LID_OPEN_ALERT_RULE)) && !m_fuzzy->isFiredRule(LID_CLOSE_ALERT_RULE);

    // We choice to control speed instead speed override because we always want a user to override
    // the speed of the fan themselve
    // When lid is detected as open we keep current fan speed
    if (m_lidOpenTriggered) {
        m_fan->speed(m_config.fan_speed_lid_open > -1 ? m_config.fan_speed_lid_open : m_fanCurrentSpeed);
    } else {
        m_fanCurrentSpeed = m_fuzzy->defuzzify(FAN_OUTPUT);
        m_fan->speed(m_fanCurrentSpeed);
    }

    // Remember temperature change
    m_tempLast = m_tempSensor->get();
}

bool BBQFanOnly::ruleFired(uint8_t i) {
    return m_fuzzy->isFiredRule(i);
}

float BBQFanOnly::tempChangeInput() const {
    return m_lastTempChange;
}

float BBQFanOnly::lastErrorInput() const {
    return m_tempLastError;
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
