#pragma once

#include <QMatrix4x4>
#include <memory>
#include <vector>

namespace rendergraph {
class Engine;
class BaseNode;
} // namespace rendergraph

class rendergraph::Engine {
  public:
    Engine(std::unique_ptr<BaseNode> pRootNode);
    ~Engine();

    void render();
    /// Set the projection for a widget of this size. With quarterTurn the
    /// scene is turned so that a node's +x (the axis its content runs
    /// along) points down the widget instead of across it.
    void resize(int w, int h, bool quarterTurn = false);
    void preprocess();
    void add(BaseNode* pNode);
    void remove(BaseNode* pNode);
    const QMatrix4x4& matrix() const {
        return m_matrix;
    }

  private:
    void render(BaseNode* pNode);
    void resize(BaseNode* pNode, int, int);

    QMatrix4x4 m_matrix;
    std::unique_ptr<BaseNode> m_pRootNode;
    std::vector<BaseNode*> m_pPreprocessNodes;
    std::vector<BaseNode*> m_pInitializeNodes;
};
