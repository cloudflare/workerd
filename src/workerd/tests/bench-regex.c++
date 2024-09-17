// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

// A benchmark for regular expressions performance. Note that this is at least in part benchmarking
// V8's Regex implementation.

namespace workerd {
namespace {

struct RegExpBenchmark: public benchmark::Fixture {
  virtual ~RegExpBenchmark() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {
    TestFixture::SetupParams params = {.mainModuleSource = R"(
        export default {
          async fetch(request, env, ctx) {
            const body = await request.text();
            // Common english language words, derived from "12dicts", which is in the public domain.
            // The first 600 words were chosen to have words with shared substrings and require compiling a longer expression.
            const dict_exp = new RegExp("^(A|a|aback|abacus|abandon|abandoned|abandonment|abashed|abate|abbey|abbr.|abbreviate|abbreviation|ABC|ABC's|abdicate|abdication|abdomen|abdominal|abduct|abduction|aberration|abet|abhor|abhorrence|abhorrent|abide|abiding|ability|abject|ablaze|able|able-bodied|ably|abnormal|abnormality|abnormally|aboard|abolish|abolition|abolitionist|abominable|aboriginal|aborigine|abort|abortion|abortive|abound|about|about-face|above|aboveboard|abrasive|abrasively|abreast|abridge|abridgment|abroad|abrupt|abruptly|abruptness|abscess|abscond|absence|absent|absentee|absenteeism|absently|absent-minded|absent-mindedly|absent-mindedness|absolute|absolutely|absolve|absorb|absorbed|absorbent|absorbing|absorption|abstain|abstention|abstinence|abstinent|abstract|abstraction|absurd|absurdity|absurdly|abundance|abundant|abundantly|abuse|abusive|abysmal|abysmally|abyss|AC|academic|academically|academy|accelerate|acceleration|accelerator|accent|accented|accentuate|accept|acceptability|acceptable|acceptably|acceptance|accepted|access|accessibility|accessible|accessory|accident|accidental|accidentally|accident-prone|acclaim|acclaimed|acclimate|acclimation|accolade|accommodate|accommodating|accommodation|accommodations|accompaniment|accompanist|accompany|accomplice|accomplish|accomplished|accomplishment|accord|accordance|accordingly|according to|accordion|accost|account|accountability|accountable|accountant|accounting|accreditation|accredited|accrue|accumulate|accumulation|accuracy|accurate|accurately|accusation|accuse|accused|accuser|accusing|accusingly|accustom|accustomed|ace|acerbic|ache|achieve|achievement|achiever|Achilles' heel|achy|acid|acidic|acidity|acid rain|acknowledge|acknowledged|acknowledgment|acne|acorn|acoustic|acoustics|acquaint|acquaintance|acquainted|acquiesce|acquiescence|acquire|acquisition|acquit|acquittal|acre|acrid|acrimonious|acrimony|acrobat|acrobatic|acrobatics|acronym|across|across from|across-the-board|acrylic|ACT|act|acting|action|activate|activation|active|activism|activist|activity|actor|actress|actual|actuality|actualization|actually|acumen|acupuncture|acute|acute angle|acutely|ad|A.D.|adage|adamant|adamantly|Adam's apple|adapt|adaptable|adaptation|adapter|add|addict|addicted|addiction|addictive|addition|additional|additionally|additive|address|adept|adeptly|adequacy|adequate|adequately|adhere|adherence|adherent|adhesion|adhesive|ad hoc|adjacent|adjectival|adjective|adjoin|adjoining|adjourn|adjournment|adjudicate|adjudicator|adjunct|adjust|adjustable|adjustment|ad lib|ad-lib|administer|administration|administrative|administrator|admirable|admirably|admiral|admiration|admire|admirer|admiring|admiringly|admissible|admission|admit|admittance|admittedly|admonish|admonition|adobe|adolescence|adolescent|adopt|adopted|adoption|adoptive|adorable|adoration|adore|adorn|adornment|adrenaline|adrift|adroit|adroitly|adulation|adult|adulterate|adulteration|adultery|advance|advanced|advancement|advantage|advantageous|Advent|advent|adventure|adventurer|adventurous|adverb|adverbial|adversary|adverse|adversely|adversity|advertise|advertisement|advertiser|advertising|advice|advisable|advise|adviser|advisory|advocacy|advocate|aerial|aerobic|aerobics|aerodynamic|aerodynamics|aerosol|aerospace|aesthetic|aesthetically|aesthetics|afar|affable|affably|affair|affairs|affect|affectation|affected|affection|affectionate|affectionately|affidavit|affiliate|affiliated|affiliation|affinity|affirm|affirmation|affirmative|affirmative action|affirmatively|affix|afflict|affliction|affluence|affluent|afford|affordable|affront|afloat|afraid|afresh|Africa|African|African-American|after|aftereffect|afterlife|aftermath|afternoon|aftershave|aftershock|afterthought|afterward|afterwards|again|against|age|aged|agency|agenda|agent|ages|aggravate|aggravating|aggravation|aggression|aggressive|aggressively|aggressiveness|aggressor|aggrieved|aghast|agile|agility|aging|agitate|agitated|agitation|agitator|agnostic|agnosticism|ago|agonize|agonized|agonizing|agonizingly|agony|agree|agreeable|agreeably|agreed|agreement|agricultural|agriculture|ah|aha|ahead|aid|aide|AIDS|ailing|ailment|aim|aimless|aimlessly|ain't|air|air bag|air base|airborne|air-conditioned|air conditioner|air conditioning|aircraft|aircraft carrier|airfare|airfield|air force|airily|airing|airless|airline|airliner|airmail|airplane|airport|air raid|airs|airspace|airstrip|airtight|air time|air traffic controller|airwaves|airy|aisle|ajar|akin|a la carte|a la mode|alarm|alarm clock|alarmed|alarming|alarmingly|alarmist|alas|albeit|albino|album|alcohol|alcoholic|alcoholism|alcove|alderman|alderwoman|ale|alert|alfalfa|algae|algebra|algebraic|algorithm|alias|alibi|alien|alienate|alienation|alight|align|alignment|alike|alimony|alive|alkali|alkaline|all|Allah|all-American|all-around|allay|all-clear|allegation|allege|alleged|allegedly|allegiance|allegorical|allegory|allergic|allergy|alleviate|alleviation|alley|alliance|allied|alligator|all-inclusive|allocate|allocation|allot|allotment|all-out|allow|allowable|allowance|alloy|all right|all-star|allude|allure|alluring|allusion|ally|alma mater|almanac|almighty|almond|almost|alms|aloft|aloha|alone|along|alongside|aloof|aloud|alphabet|alphabetical|alphabetically|alpine|already|alright|also|altar|alter|alteration|altercation|alternate|alternately|alternation|alternative|alternatively|although|altitude|alto|altogether|altruism|altruistic|aluminum|alumna|alumnae|alumni|alumnus|always|AM|am|A.M.|amalgamate|amalgamation|amass|amateur)$");
            if (dict_exp.test(body)) {
              return new Response("word found in dictionary");
            }
            return new Response("error: word not found", {status: 400});
          }
        }
      )"_kj};
    fixture = kj::heap<TestFixture>(kj::mv(params));
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    fixture = nullptr;
  }

  kj::Own<TestFixture> fixture;
};

BENCHMARK_F(RegExpBenchmark, request)(benchmark::State& state) {
  for (auto _: state) {
    auto result =
        fixture->runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, "accepted"_kj);
    KJ_EXPECT(result.statusCode == 200 && result.body == "word found in dictionary"_kj);
    auto result2 =
        fixture->runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, "invalid"_kj);
    KJ_EXPECT(result2.statusCode == 400 && result2.body == "error: word not found"_kj);
  }
}

}  // namespace
}  // namespace workerd
