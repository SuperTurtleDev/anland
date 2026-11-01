#pragma once
#include <aquamarine/input/Input.hpp>
#include <string>
#include <map>
#include "anland_tablet.h"
namespace Aquamarine {
    class CAnlandPointer : public IPointer {
      public:
        const std::string& getName() override { return m_name; }
      private:
        std::string m_name = "anland-pointer";
    };
    class CAnlandKeyboard : public IKeyboard {
      public:
        const std::string& getName() override { return m_name; }
      private:
        std::string m_name = "anland-keyboard";
    };
    class CAnlandTouch : public ITouch {
      public:
        const std::string& getName() override { return m_name; }
      private:
        std::string m_name = "anland-touch";
    };

    class CAnlandTabletTool;
    class CAnlandTablet : public ITablet {
      public:
        const std::string& getName() override { return m_name; }
        anland_tablet state{};
        std::map<uint32_t, Hyprutils::Memory::CSharedPointer<CAnlandTabletTool>> tools;
      private:
        std::string m_name = "anland-tablet";
    };
    class CAnlandTabletTool : public ITabletTool {
      public:
        const std::string& getName() override { return m_name; }
      private:
        std::string m_name = "anland-pen";
    };

}
