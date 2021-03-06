#ifndef TRANSFORM_STACK
#define TRANSFORM_STACL

#include <stack>
#include <glm/glm.hpp>

#include <iostream>
#include <playground/print.h>


class TransformStack {

    public:

        void push(glm::mat4 M);
        glm::mat4 pop();
        glm::mat4 computeTransform();

        static TransformStack& getInstance() {
            static TransformStack instance; 
            return instance;
        };

    private:

        std::stack<glm::mat4> T;

        TransformStack(){};
        TransformStack(TransformStack const&);
        void operator = (TransformStack const&);
};


#endif
