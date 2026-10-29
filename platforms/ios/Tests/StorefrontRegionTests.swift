import XCTest
@testable import RIMES

final class StorefrontRegionTests: XCTestCase {
    func testStorefrontCodesBecomeLookupCountries() {
        let pairs = ["USA": "US", "CHN": "CN", "GBR": "GB", "DEU": "DE", "JPN": "JP",
                     "SGP": "SG", "KOR": "KR", "HKG": "HK", "TWN": "TW", "ARE": "AE"]
        for (storefront, expected) in pairs {
            XCTAssertEqual(StorefrontRegion.lookupCountry(for: storefront), expected)
        }
        XCTAssertEqual(StorefrontRegion.lookupCountry(for: "gbr"), "GB")
    }
    func testUnavailableOrInvalidStorefrontNeverFallsBackToDeviceRegion() {
        let values: [String?] = [nil, "", "US", "ZZZ", "USA ", "001", "US;", "🇺🇸", "SUN"]
        for value in values {
            XCTAssertNil(StorefrontRegion.lookupCountry(for: value))
        }
    }
}
